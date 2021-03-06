#include "Bat.h"
#include "api.h"
#include "ad.h"

static int adc_buffer[10];
void Bat::Init()
{
	//int * ad_buffer = get_ad_data_buffer();
	
	sensorList.Add(&inVolt);
	sensorList.Add(&cFlyVolt);
	sensorList.Add(&outVolt);
	sensorList.Add(&batCurr);
	sensorList.Add(&iOutPeak);
	
	inVolt.Init(&adc_buffer[u_in_index], 13533, 95);
	cFlyVolt.Init(&adc_buffer[u_cfly_index], 13533, 80);
	outVolt.Init(&adc_buffer[u_out_index], 13533, 95);
	batCurr.Init(&adc_buffer[i_out_index], 4500, 95);
	iOutPeak.Init(&adc_buffer[i_out_peak_index], 4500, 95);
	igbt1Temp.Init(&adc_buffer[temp1_index], 0, 5);
	igbt2Temp.Init(&adc_buffer[temp2_index], 0, 5);
	
}

void Bat::RefreshState()
{
	Product::RefreshTransducerData(sensorList);
	
	igbt1Temp.Update();
	igbt2Temp.Update();
	
	FastCheck();
	
	UpdateFaultState();
}

void Bat::On()
{
	int iTemp;
	
	if (!isStartEnable)
	{
		Device::On();
		state = SC_SOFT_START;
	}
	
	if (isConstP)
	{
		uOutFilt = outVolt.GetAverageRealValue();
		if (uOutFilt < 1000)
		{
			iTemp = iTarget;
		}
		else
		{
			//20kw �㹦��
			iTemp = 2000000 / uOutFilt;
		}
	}
	else
	{
		iTemp = iTarget;
	}
	
	switch (state)
	{
		case SC_SOFT_START:
			iReference += 10;
			if (iTemp <= iReference)
			{
				iReference = iTemp;
				state = SC_CURR_LIMITTED;
			}
			break;
		case SC_CURR_LIMITTED:
			if (iTemp > (iReference + 5))
			{
				iReference += 5;
			}
			else if (iTemp < (iReference - 5))
			{
				iReference -= 5;
			}
			else
			{
				iReference = iTemp;
			}
			break;
		default:
			break;
	}
	
}

void Bat::Off()
{
	if (isStartEnable)
	{
		Device::Off();
		duty1 = 0;
		duty2 = 0;
		cFlyDuty = 0;
		presentDuty = 0;
		iReference = 0;
		isSteady = false;
		pidVoltage.Reset();
		pidBatteryCurrent.Reset();
		pidIOutPeak.Reset();
		pidCfly.Reset();
		relays.Reset();
	}
	fpga.duty1 = 0;
	fpga.duty2 = 0;
	
	fpga.enable = isStartEnable;
}

void Bat::Run()
{
	if (!isStartEnable)
	{
		return;
	}
	
	switch (startMode)
	{
		case SC_START_CHARGE:
			ChargeCtrl();
			break;
		case SC_START_DISCHARGE:
			DisChargeCtrl();
			break;
		default:
			break;
	}
	
	fpga.duty1 = duty1;
	fpga.duty2 = duty2;
	
	fpga.enable = isStartEnable;
	
}

int lastUIn;

int lowRate = 45;
int highRate = 55;

int lowVolt = 0;
int highVolt = 0;

void Bat::ChargeCtrl()
{
	int iBat = 0;
	int uOut = 0;
	int uIn = 0;
	int uInFilt = 0;
	int uCFly = 0;
	
	iBat = batCurr.GetIRealValue();
	uOut = outVolt.GetIRealValue();
	uOutFilt = outVolt.GetAverageRealValue();
	uIn = inVolt.GetIRealValue();
	uInFilt = inVolt.GetAverageRealValue();
	uCFly = cFlyVolt.GetIRealValue();
	
	lowVolt = ((lowRate * uInFilt+50)/100);
	highVolt = ((highRate * uInFilt+50)/100);
	
	uInDerivative = lastUIn - uIn;
	lastUIn = uIn;
	
	uInRipple = (uIn - uInFilt) * 12000 / uIn;
	if (uInRipple > 900)
	{
		uInRipple = 900;
	}
	else if (uInRipple < -900)
	{
		uInRipple = -900;
	}
	
	filterDuty = (filterDuty * 4 + duty1 + 2) / 5;
	if(filterDuty < 4500)
    {
        pidBatteryCurrent.proportionalGain = maxPGain;
    }
    else if(filterDuty > 13500)
    {
        pidBatteryCurrent.proportionalGain = minPGain;
    }
    else
    {
        int k = (minPGain - maxPGain) / 2;
        
        pidBatteryCurrent.proportionalGain = filterDuty * k / 4500 + maxPGain - k;
    }
	
	pidVoltage.feedback = uOut;
    pidVoltage.Update();
    
    pidIOutPeak.reference = iOutPeak.GetAverageRealValue();
    pidIOutPeak.feedback =  iOutPeak.GetIRealValue();
    pidIOutPeak.Update(0);

    pidBatteryCurrent.reference = iReference;
    pidBatteryCurrent.feedback = iBat;
    pidBatteryCurrent.Update();
    
    pidCfly.reference = 0;
    pidCfly.feedback = uCFly - (uInFilt / 2);
    pidCfly.Update();
	
	dutyVoltage = pidVoltage.out / 200;
    dutyBattary = pidBatteryCurrent.out / 200;
    cFlyDuty    = pidCfly.out / 200;
    iPeakDuty    = pidIOutPeak.out;
	
	if (dutyBattary < dutyVoltage)
	{
		//��������
        presentDuty = dutyBattary;
        pidVoltage.Reset(presentDuty * 200);
        isSteady = true;
	}
	else
	{
		//��ѹ����
        presentDuty = dutyVoltage;
        iPeakDuty = 0;
        pidBatteryCurrent.Reset(presentDuty * 200);
	}
	
	if (presentDuty > 500)
	{
		presentDuty -= (uInRipple / kpInRipple + uInDerivative * uOutFilt / 1400);
	}
	
	if (presentDuty > 1000)
	{
		presentDuty += iPeakDuty;
	}
	
	if (presentDuty > thresholdDuty)
	{
		presentDuty = thresholdDuty;
	}

	duty1 = presentDuty;
	
	if (duty1 >= 432)
	{
		int tmpDuty = cFlyDuty;
		int flag = 1;
		if (tmpDuty < 0)
		{
			flag = -1;
			tmpDuty = -tmpDuty;
		}
		
		if(tmpDuty > duty1 / 10)
		{
			duty2 = duty1 - duty1 * flag / 10;
		}
		else
		{
			duty2 = duty1 - cFlyDuty;
		}
	}
	else
	{
		duty2 = duty1;
		
		if(uCFly < lowVolt)//�ɿ����ƫ��
		{
			if(++cutTime >= 5)
			{
				cutTime = 0;
				duty1 = 360;//432->6us   216-->3us
				duty2 = 0;
			}
		}
		else if(uCFly > highVolt)//�ɿ����ƫ��
		{
			if(++cutTime >= 5)
			{
				cutTime = 0;
				duty1 = 0;//432->6us   216-->3us
				duty2 = 360;
			}
		}
		else
		{
			duty1 = 0; 
			duty2 = 0;
			cutTime = 0;
		}
	}
}

void Bat::DisChargeCtrl()
{
	int iBat = 0;
	int uIn = 0;
	int uInFilt = 0;
	int uCFly = 0;
	
	iBat = batCurr.GetIRealValue();
	uOutFilt = outVolt.GetAverageRealValue();
	uIn = inVolt.GetIRealValue();
	uInFilt = inVolt.GetAverageRealValue();
	uCFly = cFlyVolt.GetIRealValue();
	
	pidVoltage.feedback = uIn;
    pidVoltage.Update();

    pidBatteryCurrent.reference = iReference;
    pidBatteryCurrent.feedback = -iBat;
    pidBatteryCurrent.Update();
    
    pidCfly.reference = 0;
    pidCfly.feedback = uCFly - (uInFilt / 2);
    pidCfly.Update();
	
	dutyVoltage = pidVoltage.out / 200;
    dutyBattary = pidBatteryCurrent.out / 200;
    cFlyDuty    = pidCfly.out / 200;
	
	if (dutyBattary < dutyVoltage)
	{
		//��������
        presentDuty = dutyBattary;
        pidVoltage.Reset(presentDuty * 200);
        isSteady = true;
	}
	else
	{
		//��ѹ����
        presentDuty = dutyVoltage;
        iPeakDuty = 0;
        pidBatteryCurrent.Reset(presentDuty * 200);
	}
	
	if (presentDuty > thresholdDuty)
	{
		presentDuty = thresholdDuty;
	}

	duty1 = presentDuty;
	
	if (duty1 >= 432)
	{
		int tmpDuty = cFlyDuty;
		int flag = 1;
		if (tmpDuty < 0)
		{
			flag = -1;
			tmpDuty = -tmpDuty;
		}
		
		if(tmpDuty > duty1 / 10)
		{
			duty2 = duty1 - duty1 * flag / 10;
		}
		else
		{
			duty2 = duty1 - cFlyDuty;
		}
	}
	else
	{
		duty1 = 0; 
		duty2 = 0;
	}
}

void Bat::FaultCheckModuleInit()
{
	failureList.Add(&uInMaxSlow);
	failureList.Add(&uCfly);
	failureList.Add(&iBatMaxSlow);
	failureList.Add(&iBatMinSlow);
	failureList.Add(&uBatMaxSlow);
	failureList.Add(&uInBatSlow);
	failureList.Add(&igbt1TempOver);
	failureList.Add(&igbt2TempOver);
	failureList.Add(&iBatMaxHW);
	failureList.Add(&uFlyMaxHW);
	failureList.Add(&uBatMaxHW);
	
	failureList.Add(&uInMaxFast);
	failureList.Add(&iBatMaxFast);
	failureList.Add(&iBatMinFast);
	failureList.Add(&uBatMaxFast);
	failureList.Add(&uInBatFast);
	
	failureList.Add(&igbt1TempFail);
	failureList.Add(&igbt2TempFail);
	
    relays.Register(&iBatMin1HoldTime);
	relays.Register(&iBatMin2HoldTime);
    relays.Register(&iBatMax1HoldTime);
	relays.Register(&iBatMax2HoldTime);
    relays.Register(&uBatMax1HoldTime);
	relays.Register(&uBatMax2HoldTime);
    relays.Register(&uInMax1HoldTime);
	relays.Register(&uInMax2HoldTime);
	relays.Register(&uInBat1HoldTime);
	relays.Register(&uInBat2HoldTime);
	relays.Register(&igbt1TOver1HoldTime);
    relays.Register(&igbt1TOver2HoldTime);
	relays.Register(&igbt2TOver1HoldTime);
    relays.Register(&igbt2TOver2HoldTime);
	relays.Register(&uCFly1HoldTime);
    relays.Register(&uCFly2HoldTime);
	
	relays.Register(&igbt1TempFailHoldTime);
    relays.Register(&igbt2TempFailHoldTime);
}

void Bat::FastCheck()
{
	if (!isStartEnable)
	{
		return;
	}
	UInCheckFast();
	IOutCheckFast();
	UOutCheckFast();
	IOCheck();
}

void Bat::SlowCheck()
{
	if (isStartEnable)
	{
		IOutCheck();
		UCflyCheck();
	}
	UInCheck();
	//UOutCheck();
	TempFailCheck();
	TempCheck();
}

void Bat::UInCheckFast()
{
	int uInValue = inVolt.GetInstantaneousValue();
	//int uInOver = scData->cbPara.dischargeVoltOverW;

	if (IsMaximizing(uInValue, 2050))
	{
		uInMaxCount.Increase();
		if (uInMaxCount.result)
		{
			uInMaxFast.Encounter();
		}
	}
	else
	{
		uInMaxCount.Reset();
	}
	
	if (IsMaximizing(uInValue, 2050))
	{
		uInBatCount.Increase();
		if (uInBatCount.result)
		{
			uInBatFast.Encounter();
		}
	}
	else
	{
		uInBatCount.Reset();
	}
}

void Bat::IOutCheckFast()
{
	int iOutValue = batCurr.GetInstantaneousValue();
	int iOutMax = scData->cbPara.chargeCurrOverW;
	//int iOutMin = scData->cbPara.dischargeCurrOverW;
	
	if (IsMaximizing(iOutValue, iOutMax))
	{
		iBatMaxCount.Increase();
		if (iBatMaxCount.result)
		{
			iBatMaxFast.Encounter();
		}
	}
	else if (IsMaximizing(iOutValue, 2700))
	{
		iBatMaxCount.Increase();
		if (iBatMaxCount.result)
		{
			iBatMaxFast.Encounter();
		}
	}
	else
	{
		iBatMaxCount.Reset();
	}
	
	if (IsMaximizing(-iOutValue, 2500))
	{
		iBatMinCount.Increase();
		if (iBatMinCount.result)
		{
			iBatMinFast.Encounter();
		}
	}
	else
	{
		iBatMinCount.Reset();
	}
}

void Bat::UOutCheckFast()
{
	int uOutValue = outVolt.GetInstantaneousValue();
	int uOutOver = scData->cbPara.chargeVoltOverW;
	
	if (IsMaximizing(uOutValue, uOutOver))
	{
		uBatMaxCount.Increase();
		if (uBatMaxCount.result)
		{
			uBatMaxFast.Encounter();
		}
	}
	else if (IsMaximizing(uOutValue, 9200))
	{
		uBatMaxCount.Increase();
		if (uBatMaxCount.result)
		{
			uBatMaxFast.Encounter();
		}
	}
	else
	{
		uBatMaxCount.Reset();
	}	
}

//Ӳ�����ϣ�fpga����
void Bat::IOCheck()
{
	if (fpga.i_max)
	{
		iBatMaxHW.Encounter();
	}
	
	if (fpga.u_out_max)
	{
		uBatMaxHW.Encounter();
	}
	
	if (fpga.u_fly_max)
	{
		uFlyMaxHW.Encounter();
	}
}

void Bat::UInCheck()
{
	int uInValue = scData->ad.u_in;
	//int uInOver = scData->cbPara.dischargeVoltOverW;
	
	bool max1 = IsMaximizing(uInValue, 1900, &uInMax1HoldTime);
	bool max2 = IsMaximizing(uInValue, 2050, &uInMax2HoldTime);
	
	if (max1 || max2)
	{
		uInMaxSlow.Encounter();
	}
}

void Bat::UCflyCheck()
{
	int uInValue = scData->ad.u_in;
	int uCflyValue = scData->ad.u_cfly;
	int uCflyOver = scData->cbPara.flyingCapVoltOverW;
	
	bool max1 = IsMaximizing(uInValue - uCflyValue, uCflyOver, &uCFly1HoldTime);
	bool max2 = IsMaximizing(uCflyValue, 1100, &uCFly2HoldTime);
	bool max3 = IsMaximizing(uInValue - uCflyValue, 1300, &uCFly2HoldTime);
	
	if (max1 || max2 || max3)
	{
		uCfly.Encounter();
	}
}

void Bat::IOutCheck()
{
	int iOutValue = scData->ad.i_out;
	int iOutMax = scData->cbPara.chargeCurrOverW;
	int iOutMin = scData->cbPara.dischargeCurrOverW;
	
	bool max1 = IsMaximizing(iOutValue, iOutMax, &iBatMax1HoldTime);
	bool max2 = IsMaximizing(iOutValue, 2700, &iBatMax2HoldTime);
	bool max3 = IsMaximizing(-iOutValue, iOutMin, &iBatMin1HoldTime);
	bool max4 = IsMaximizing(-iOutValue, 2700, &iBatMin2HoldTime);
	
	if (max1 || max2)
	{
		iBatMaxSlow.Encounter();
	}
	else if (max3 || max4)
	{
		iBatMinSlow.Encounter();
	}
}

void Bat::UOutCheck()
{
	int uOutValue = scData->ad.u_out;
	int uOutOver = scData->cbPara.chargeVoltOverW;
	
	bool max1 = IsMaximizing(uOutValue, uOutOver, &uBatMax1HoldTime);
	bool max2 = IsMaximizing(uOutValue, 9200, &uBatMax2HoldTime);
	
	if (max1 || max2)
	{
		uBatMaxFast.Encounter();
	}
}

void Bat::TempFailCheck()
{
	int temp1 = scData->ad.temp_igpt1;
	int temp2 = scData->ad.temp_igpt2;
	
	bool max1 = IsMinimizing(temp1, -39, &igbt1TempFailHoldTime);
	bool max2 = IsMinimizing(temp2, -39, &igbt2TempFailHoldTime);
	
	if (max1)
	{
		igbt1TempFail.Encounter();
	}
	
	if (max2)
	{
		igbt2TempFail.Encounter();
	}
}

void Bat::TempCheck()
{
	int temp1 = scData->ad.temp_igpt1;
	int temp2 = scData->ad.temp_igpt2;
	int tmep1Max = scData->cbPara.IGBT1TemprOver;
	int tmep2Max = scData->cbPara.IGBT2TemprOver;
	
	bool max1 = IsMaximizing(temp1, tmep1Max, &igbt1TOver1HoldTime);
	bool max2 = IsMaximizing(temp1, 85, &igbt1TOver2HoldTime);
	bool max3 = IsMaximizing(temp2, tmep2Max, &igbt2TOver1HoldTime);
	bool max4 = IsMaximizing(temp2, 85, &igbt2TOver2HoldTime);
	
	if (max1 || max2)
	{
		igbt1TempOver.Encounter();
	}
	
	if (max3 || max4)
	{
		igbt2TempOver.Encounter();
	}
}

void Bat::UpdateFaultState()
{
	FailureCheck::UpdateFaultState();
}

void Bat::RefreshRelay()
{
	relays.Refresh();
}

void Bat::SetSharedData(void * sharedData)
{
	this->scData = (ScData *)sharedData;
}
