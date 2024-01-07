/*
 * Userspace driver for Tiqiaa Tview USB IR Transeiver
 *
 * Copyright (c) Xen xen-re[at]tutanota.com
 */

#include "TiqiaaUsb.h"
#include <setupapi.h>

#ifndef GUID_DEVINTERFACE_USB_DEVICE
DEFINE_GUID( GUID_DEVINTERFACE_USB_DEVICE, 0xA5DCBF10L, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED );
#endif

TiqiaaUsbIr::TiqiaaUsbIr(){
	DevHandle = INVALID_HANDLE_VALUE;
	DevWinUsbHandle = NULL;
	IrRecvCallback = NULL;
	IrRecvCbContext = NULL;
	PacketIndex = 0;
	CmdId = 0;
	DeviceState = 0;
	InitializeCriticalSection(&WaitCmdCs);
	WaitCmdEvent = CreateEvent(NULL, false, false, NULL);
}

TiqiaaUsbIr::~TiqiaaUsbIr(){
	Close();
	CloseHandle(WaitCmdEvent);
	DeleteCriticalSection(&WaitCmdCs);
}

bool TiqiaaUsbIr::Open(const char * device_path){
	if (IsOpen()) return false;
	DevHandle = CreateFileA(device_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (DevHandle == INVALID_HANDLE_VALUE) return false;
	if (WinUsb_Initialize(DevHandle, &DevWinUsbHandle)){
		IsWaitingCmdReply = false;
		ReadActive = true;
		ReadThreadHandle = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)RunReadThreadFn, this, 0, &ReadThreadId);
		if (ReadThreadHandle != NULL){
			if (SendCmdAndWaitReply(CmdVersion, GetCmdId(), CmdReplyWaitTimeout)){
				if (SendCmdAndWaitReply(CmdSendMode, GetCmdId(), CmdReplyWaitTimeout)){
					return true;
				}
			}
			ReadActive = false;
			WinUsb_AbortPipe(DevWinUsbHandle, ReadPipeId);
			WaitForSingleObject(ReadThreadHandle, INFINITE);
		}
		WinUsb_Free(DevWinUsbHandle);
	}
	CloseHandle(DevHandle);
	DevHandle = INVALID_HANDLE_VALUE;
	return false;
}

bool TiqiaaUsbIr::Close(){
	if (!IsOpen()) return false;
	SetIdleMode();
	ReadActive = false;
	WinUsb_AbortPipe(DevWinUsbHandle, ReadPipeId);
	WinUsb_Free(DevWinUsbHandle);
	WaitForSingleObject(ReadThreadHandle, INFINITE);
	CloseHandle(DevHandle);
	DevHandle = INVALID_HANDLE_VALUE;
	return true;
}

bool TiqiaaUsbIr::IsOpen(){
	return (DevHandle != INVALID_HANDLE_VALUE);
}

bool TiqiaaUsbIr::SendReport2(void * data, int size){
	uint8_t FragmBuf[61];
	TiqiaaUsbIr_Report2Header * ReportHdr = (TiqiaaUsbIr_Report2Header *)FragmBuf;
	int RdPtr;
	int FragmIndex;
	int FragmSize;
	ULONG UsbTxSize;

	RdPtr = 0;
	if ((size <= 0) || (size > MaxUsbPacketSize)) return false;
	memset(FragmBuf, 0 ,sizeof(FragmBuf));
	ReportHdr->ReportId = WriteReportId;
	ReportHdr->FragmCount = size / MaxUsbFragmSize;
	if ((size % MaxUsbFragmSize) != 0) ReportHdr->FragmCount ++;
	PacketIndex ++;
	if (PacketIndex > MaxUsbPacketIndex) PacketIndex = 1;
	ReportHdr->PacketIdx = PacketIndex;
	FragmIndex = 0;
	while (RdPtr < size){
		FragmIndex ++;
		ReportHdr->FragmIdx = FragmIndex;
		FragmSize = size - RdPtr;
		if (FragmSize > MaxUsbFragmSize) FragmSize = MaxUsbFragmSize;
		ReportHdr->FragmSize = FragmSize + 3;
		memcpy(FragmBuf + sizeof(TiqiaaUsbIr_Report2Header), ((uint8_t *)data) + RdPtr, FragmSize);
		if (!WinUsb_WritePipe(DevWinUsbHandle, WritePipeId, FragmBuf, FragmSize + sizeof(TiqiaaUsbIr_Report2Header), &UsbTxSize, NULL)) return false;
		RdPtr += FragmSize;
	}
	return true;
}

bool TiqiaaUsbIr::SendCmd(uint8_t cmdType, uint8_t cmdId){
	TiqiaaUsbIr_SendCmdPack Pack;

	Pack.StartSign = PackStartSign;
	Pack.CmdType = cmdType;
	Pack.CmdId = cmdId;
	Pack.EndSign = PackEndSign;
	return SendReport2(&Pack, sizeof(Pack));
}

bool TiqiaaUsbIr::SendIRCmd(int freq, void * buffer, int buf_size, uint8_t cmdId){
	uint8_t PackBuf[MaxUsbPacketSize];
	TiqiaaUsbIr_SendIRPackHeader * PackHeader = (TiqiaaUsbIr_SendIRPackHeader *)PackBuf;
	uint8_t IrFreqId;
	int PackSize = sizeof(TiqiaaUsbIr_SendIRPackHeader);

	if (buf_size < 0) return false;
	if ((buf_size + sizeof(TiqiaaUsbIr_SendIRPackHeader) + sizeof(uint16_t)) > MaxUsbPacketSize) return false;
	if (freq > 255){
		IrFreqId = 0;
		while ((IrFreqId < TiqiaaUsbIr_IrFreqTableSize) && (TiqiaaUsbIr_IrFreqTable[IrFreqId] != freq)) IrFreqId++;
		if (IrFreqId >= TiqiaaUsbIr_IrFreqTableSize) return false;
	} else {
		if (freq < TiqiaaUsbIr_IrFreqTableSize) IrFreqId = freq; else return false;
	}
	PackHeader->StartSign = PackStartSign;
	PackHeader->CmdType = 'D';
	PackHeader->CmdId = cmdId;
	PackHeader->IrFreqId = IrFreqId;
	memcpy(PackBuf + PackSize, buffer, buf_size);
	PackSize += buf_size;
	*(uint16_t *)(PackBuf + PackSize) = PackEndSign;
	PackSize += sizeof(uint16_t);
	return SendReport2(PackBuf, PackSize);
}

bool TiqiaaUsbIr::SendCmdAndWaitReply(uint8_t cmdType, uint8_t cmdId, DWORD timeout){
	if (!StartCmdReplyWaiting(cmdType, cmdId)) return false;
	if (SendCmd(cmdType, cmdId)){
		if (WaitCmdReply(timeout)) return true;
	}
	CancelCmdReplyWaiting();
	return false;
}

uint8_t TiqiaaUsbIr::GetCmdId(){
	if (CmdId < MaxCmdId) CmdId ++; else CmdId = 1;
	return CmdId;
}

bool TiqiaaUsbIr::StartCmdReplyWaiting(uint8_t cmdType, uint8_t cmdId){
	if (!IsOpen()) return false;
	EnterCriticalSection(&WaitCmdCs);
	if (IsWaitingCmdReply) return false;
	WaitCmdId = cmdId;
	WaitCmdType = cmdType;
	IsWaitingCmdReply = true;
	IsCmdReplyReceived = false;
	LeaveCriticalSection(&WaitCmdCs);
	return true;
}

bool TiqiaaUsbIr::WaitCmdReply(DWORD timeout){
	bool res = false;
	if (!IsOpen()) return false;
	if (!IsWaitingCmdReply) return false;
	WaitForSingleObject(WaitCmdEvent, timeout);
	EnterCriticalSection(&WaitCmdCs);
	if (IsWaitingCmdReply && IsCmdReplyReceived){
		res = true;
		IsWaitingCmdReply = false;
	}
	LeaveCriticalSection(&WaitCmdCs);
	return res;
}

bool TiqiaaUsbIr::CancelCmdReplyWaiting(){
	bool res = false;
	if (!IsOpen()) return false;

	EnterCriticalSection(&WaitCmdCs);
	if (IsWaitingCmdReply){
		IsWaitingCmdReply = false;
		res = true;
	}
	LeaveCriticalSection(&WaitCmdCs);
	return res;
}

bool TiqiaaUsbIr::SetIdleMode(){
	if (!IsOpen()) return false;
	if (DeviceState == StateIdle) return true;
	if (SendCmdAndWaitReply(CmdIdleMode, GetCmdId(), CmdReplyWaitTimeout)){
		if (DeviceState == StateIdle) return true;
	}
	return false;
}

bool TiqiaaUsbIr::SendIR(int freq, void * buffer, int buf_size){
	if (!IsOpen()) return false;
	if (DeviceState != StateSend){
		if (!SendCmdAndWaitReply(CmdSendMode, GetCmdId(), CmdReplyWaitTimeout)) return false;
	}
	if (DeviceState != StateSend) return false;
	uint8_t SendIRCmdId = GetCmdId();
	if (!StartCmdReplyWaiting(CmdOutput, SendIRCmdId)) return false;
	if (SendIRCmd(freq, buffer, buf_size, SendIRCmdId)){
		if (WaitCmdReply(IrReplyWaitTimeout)) return true;
	}
	CancelCmdReplyWaiting();
	return false;
}

bool TiqiaaUsbIr::StartRecvIR(){
	if (!IsOpen()) return false;
	if (DeviceState != StateRecv){
		if (!SendCmdAndWaitReply(CmdRecvMode, GetCmdId(), CmdReplyWaitTimeout)) return false;
		if (DeviceState != StateRecv) return false;
		if (!SendCmdAndWaitReply(CmdCancel, GetCmdId(), CmdReplyWaitTimeout)) return false;
	}
	if (!SendCmd(CmdOutput, GetCmdId())) return false;
	return true;
}

bool TiqiaaUsbIr::SendNecSignal(uint16_t IrCode){
	uint8_t Buf[128];
	int BufSize;

	BufSize = WriteIrNecSignal(IrCode, Buf);
	return SendIR(38000, Buf, BufSize);
}


void TiqiaaUsbIr::WriteIrNecSignalPulse(TqIrWriteData * IrWrData, int PulseCount, bool isSet){
	int TickCount;
	int SendBlockSize;

	IrWrData->PulseTime += PulseCount * NecPulseSize;
	TickCount = IrWrData->PulseTime - IrWrData->SenderTime;
	TickCount /= IrSendTickSize;
	IrWrData->SenderTime += TickCount * IrSendTickSize;
	while (TickCount > 0){
		SendBlockSize = TickCount;
		if (SendBlockSize > MaxIrSendBlockSize) SendBlockSize = MaxIrSendBlockSize;
		TickCount -= SendBlockSize;
		if (isSet) SendBlockSize |= 0x80;
		IrWrData->Buf[IrWrData->Size] = SendBlockSize;
		IrWrData->Size ++;
	}
}

int TiqiaaUsbIr::WriteIrNecSignal(uint16_t IrCode, uint8_t * OutBuf){
	TqIrWriteData WriteData;
	int i;
	uint32_t tcode;

	WriteData.Buf = OutBuf;
	WriteData.Size = 0;
	WriteData.PulseTime = 0;
	WriteData.SenderTime = 0;

	((uint8_t *)(&tcode))[0] = ((uint8_t *)(&IrCode))[1];
	((uint8_t *)(&tcode))[1] = ~((uint8_t *)(&IrCode))[1];
	((uint8_t *)(&tcode))[2] = ((uint8_t *)(&IrCode))[0];
	((uint8_t *)(&tcode))[3] = ~((uint8_t *)(&IrCode))[0];

	WriteIrNecSignalPulse(&WriteData, 16, true);
	WriteIrNecSignalPulse(&WriteData, 8, false);

	for (i=0; i<32;i++){
		WriteIrNecSignalPulse(&WriteData, 1, true);
		WriteIrNecSignalPulse(&WriteData, ((tcode&1) != 0) ? 3 : 1, false);
		tcode >>= 1;
	}
	WriteIrNecSignalPulse(&WriteData, 1, true);
	WriteIrNecSignalPulse(&WriteData, 72, false);
	return WriteData.Size;
}


void TiqiaaUsbIr::ProcessRecvPacket(uint8_t * pack, int size){
	if (IsWaitingCmdReply){
		EnterCriticalSection(&WaitCmdCs);
		if (IsWaitingCmdReply && !IsCmdReplyReceived){
			if ((pack[0] == WaitCmdId) && (pack[1] == WaitCmdType)){
				IsCmdReplyReceived = true;
				SetEvent(WaitCmdEvent);
			}
		}
		LeaveCriticalSection(&WaitCmdCs);
	}
	switch (pack[1]){
		case CmdVersion:
			if (size == (sizeof(TiqiaaUsbIr_VersionPacket) + 2)){
				TiqiaaUsbIr_VersionPacket * version = (TiqiaaUsbIr_VersionPacket *)(pack + 2);
				DeviceState = version->State;
			}
			break;
		case CmdIdleMode:
		case CmdSendMode:
		case CmdRecvMode:
		case CmdOutput:
		case CmdCancel:
		case CmdUnknown:
			DeviceState = pack[2];
			break;
		case CmdData:
			TiqiaaUsbIr_IrRecvCallback * RecvCallback = IrRecvCallback;
			if (RecvCallback) RecvCallback(pack + 2, size - 2, this, IrRecvCbContext);
			break;
	}
}

DWORD WINAPI TiqiaaUsbIr::RunReadThreadFn(TiqiaaUsbIr * cls)
{
	if (cls != NULL) cls->ReadThreadFn();
	return 0;
}

void TiqiaaUsbIr::ReadThreadFn(){
	uint8_t FragmBuf[61];
	uint8_t PackBuf[MaxUsbPacketSize];
	int PackSize;
	int FragmSize;
	uint8_t PacketIdx;
	uint8_t FragmCount;
	uint8_t LastFragmIdx = 0;
	TiqiaaUsbIr_Report2Header * ReportHdr = (TiqiaaUsbIr_Report2Header *)FragmBuf;
	ULONG UsbRxSize;

	FragmCount = 0; //not receiving packet
	while (ReadActive){
		if (WinUsb_ReadPipe(DevWinUsbHandle, ReadPipeId, FragmBuf, 61, &UsbRxSize, NULL)){
			if ((UsbRxSize > sizeof(TiqiaaUsbIr_Report2Header)) && (ReportHdr->ReportId == ReadReportId) && ((ULONG)(ReportHdr->FragmSize + 2) <= UsbRxSize)){
				if (FragmCount){//adding data to existing packet
					if ((ReportHdr->PacketIdx == PacketIdx) && (ReportHdr->FragmCount == FragmCount) && (ReportHdr->FragmIdx == (LastFragmIdx + 1))){
						LastFragmIdx ++;
					} else {//wrong fragment - drop packet
						FragmCount = 0;
					}
				}
				if (FragmCount == 0){//new packet
					if ((ReportHdr->FragmCount > 0) && (ReportHdr->FragmIdx == 1)){
						PacketIdx = ReportHdr->PacketIdx;
						FragmCount = ReportHdr->FragmCount;
						LastFragmIdx = LastFragmIdx;
						PackSize = 0;
						LastFragmIdx = 1;
					}
				}
				if (FragmCount){
					FragmSize = ReportHdr->FragmSize + 2 - sizeof(TiqiaaUsbIr_Report2Header);
					if ((PackSize + FragmSize) <= MaxUsbPacketSize){
						memcpy(PackBuf + PackSize, FragmBuf + sizeof(TiqiaaUsbIr_Report2Header), FragmSize);
						PackSize += FragmSize;
						if ((ReportHdr->FragmIdx == LastFragmIdx) && (PackSize > 6)){
							if ((*((uint16_t *)(PackBuf)) == PackStartSign) && (*((uint16_t *)(PackBuf + PackSize - 2)) == PackEndSign)){
								ProcessRecvPacket(PackBuf + 2, PackSize - 4);
							}
						}
					} else {//buffer overflow - drop packet
						FragmCount = 0;
					}
				}
			}
		}
	}
}

bool GetVidPidFromDevicePath(const char * dev_path, uint16_t * vid, uint16_t * pid){
	const char * VidStr;
	const char * PidStr;
	char ValStr[5];

	VidStr = strstr(dev_path, "vid_");
	if (VidStr == NULL) VidStr = strstr(dev_path, "VID_");
	if (VidStr == NULL) return false;
	PidStr = strstr(dev_path, "pid_");
	if (PidStr == NULL) PidStr = strstr(dev_path, "PID_");
	if (PidStr == NULL) return false;
	if ((PidStr - VidStr) != 9) return false;
	if (strlen(PidStr) < 8) return false;
	memcpy(ValStr, VidStr + 4, 4);
	*vid = (uint16_t)strtol(ValStr, NULL, 16);
	memcpy(ValStr, PidStr + 4, 4);
	*pid = (uint16_t)strtol(ValStr, NULL, 16);
	return true;
}

bool TiqiaaUsbIr::EnumDevices(std::vector<std::string> &DevList){
	const GUID * ClassGuid = &GUID_DEVINTERFACE_USB_DEVICE;
	HDEVINFO deviceInfoSet;
	SP_DEVINFO_DATA deviceInfoData;
	SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
	PSP_DEVICE_INTERFACE_DETAIL_DATA_A deviceInterfaceDetailData;
	DWORD deviceInterfaceDetailSize;
	DWORD devIntId;
	BOOL EnumDevIntRes;
	uint16_t Vid, Pid;

	deviceInfoSet = SetupDiGetClassDevs ( ClassGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	if (INVALID_HANDLE_VALUE == deviceInfoSet)
	{
		return FALSE;
	}

	devIntId = 0;
	EnumDevIntRes = TRUE;
	deviceInterfaceData.cbSize = sizeof (SP_DEVICE_INTERFACE_DATA);
	deviceInfoData.cbSize = sizeof (SP_DEVINFO_DATA);
	while (EnumDevIntRes){
		EnumDevIntRes = SetupDiEnumDeviceInterfaces (deviceInfoSet, 0, ClassGuid, devIntId, &deviceInterfaceData);
		if (EnumDevIntRes){
			deviceInterfaceDetailSize = 0;
			SetupDiGetDeviceInterfaceDetail (deviceInfoSet, &deviceInterfaceData, NULL, 0, &deviceInterfaceDetailSize, NULL);
			if ((GetLastError() == ERROR_INSUFFICIENT_BUFFER) && (deviceInterfaceDetailSize > 0)){
				deviceInterfaceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)new BYTE[deviceInterfaceDetailSize];
				memset(deviceInterfaceDetailData, 0, deviceInterfaceDetailSize);
				deviceInterfaceDetailData->cbSize = sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA_A);
				if (SetupDiGetDeviceInterfaceDetailA (deviceInfoSet, &deviceInterfaceData, deviceInterfaceDetailData, deviceInterfaceDetailSize, &deviceInterfaceDetailSize, &deviceInfoData)){
					GetVidPidFromDevicePath(deviceInterfaceDetailData->DevicePath, &Vid, &Pid);
					if (((Vid == DeviceVid1) || (Vid == DeviceVid2)) && (Pid == DevicePid)) DevList.push_back(std::string(deviceInterfaceDetailData->DevicePath));
				}
				delete deviceInterfaceDetailData;
			}
		} else {
			EnumDevIntRes = (GetLastError() != ERROR_NO_MORE_ITEMS);
		}
		devIntId ++;
	}
	SetupDiDestroyDeviceInfoList (deviceInfoSet);
	return true;
}
