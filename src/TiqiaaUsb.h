/*
 * Userspace driver for Tiqiaa Tview USB IR Transeiver
 *
 * Copyright (c) Xen xen-re[at]tutanota.com
 *
 * Example:
 *
 * std::vector<std::string> IrDev;
 * TiqiaaUsbIr::EnumDevices(IrDev);
 * TiqiaaUsbIr Ir;
 * Ir.Open(IrDev[0].c_str());
 * Ir.SendNecSignal(0x1234);
 * Ir.Close();
 */

#ifndef TIQIAA_USB_H
#define TIQIAA_USB_H

#include <stdint.h>
#include <windows.h>
#include <winusb.h>
#include <vector>
#include <string>

#pragma pack(1)

struct TiqiaaUsbIr_Report2Header{
	uint8_t ReportId;
	uint8_t FragmSize;
	uint8_t PacketIdx;
	uint8_t FragmCount;
	uint8_t FragmIdx;
};

struct TiqiaaUsbIr_SendCmdPack{
	uint16_t StartSign;
	uint8_t CmdId;
	uint8_t CmdType;
	uint16_t EndSign;
};

struct TiqiaaUsbIr_SendIRPackHeader{
	uint16_t StartSign;
	uint8_t CmdId;
	uint8_t CmdType;
	uint8_t IrFreqId;
};

struct TiqiaaUsbIr_VersionPacket{
	uint8_t VersionChar;
	uint8_t VersionInt;
	uint8_t VersionGuid[0x24];
	uint8_t State;
};

struct TqIrWriteData{
	uint8_t * Buf;
	int Size;
	int PulseTime;
	int SenderTime;
};

#pragma pack()

//send tick = 16mks, freq = 36700 hz 36.64 meas

const int TiqiaaUsbIr_IrFreqTableSize = 30;
const int TiqiaaUsbIr_IrFreqTable[TiqiaaUsbIr_IrFreqTableSize] = {
38000, 37900, 37917, 36000, 40000, 39700, 35750, 36400, 36700, 37000,
37700, 38380, 38400, 38462, 38740, 39200, 42000, 43600, 44000, 33000,
33500, 34000, 34500, 35000, 40500, 41000, 41500, 42500, 43000, 45000};

typedef void TiqiaaUsbIr_IrRecvCallback(uint8_t * data, int size, class TiqiaaUsbIr * IrCls, void * context);

class TiqiaaUsbIr {
	private:
	static const uint16_t DeviceVid1 = 0x10C4;
	static const uint16_t DeviceVid2 = 0x45E;
	static const uint16_t DevicePid = 0x8468;

	static const uint8_t CmdUnknown = 'H';
	static const uint8_t CmdVersion = 'V';
	static const uint8_t CmdIdleMode = 'L';
	static const uint8_t CmdSendMode = 'S';
	static const uint8_t CmdRecvMode = 'R';
	static const uint8_t CmdData = 'D';
	static const uint8_t CmdOutput = 'O';
	static const uint8_t CmdCancel = 'C';

	static const uint8_t StateIdle = 3;
	static const uint8_t StateSend = 9;
	static const uint8_t StateRecv = 19;

	static const int MaxUsbFragmSize = 56;
	static const int MaxUsbPacketSize = 1024;
	static const int MaxUsbPacketIndex = 15;
	static const int MaxCmdId = 0x7F;
	static const uint16_t PackStartSign = 'TS'; //"ST"
	static const uint16_t PackEndSign = 'NE'; //"EN"
	static const UCHAR WritePipeId = 1;
	static const UCHAR ReadPipeId = 0x81;
	static const uint8_t WriteReportId = 2;
	static const uint8_t ReadReportId = 1;
	static const DWORD CmdReplyWaitTimeout = 500;
	static const DWORD IrReplyWaitTimeout = 2000;

	static const int NecPulseSize = 1125; //562.5 mks
	static const int IrSendTickSize = 32; //16 mks
	static const int MaxIrSendBlockSize = 127; //ticks

	HANDLE DevHandle;
	WINUSB_INTERFACE_HANDLE DevWinUsbHandle;
	HANDLE ReadThreadHandle;
	DWORD ReadThreadId;
	bool ReadActive;
	uint8_t DeviceState;
	CRITICAL_SECTION WaitCmdCs;
	HANDLE WaitCmdEvent;

	uint8_t PacketIndex;
	uint8_t CmdId;
	bool IsWaitingCmdReply;
	bool IsCmdReplyReceived;
	uint8_t WaitCmdId;
	uint8_t WaitCmdType;

	public:

	//! Callback function for received IR signal
	TiqiaaUsbIr_IrRecvCallback * IrRecvCallback;

	//! Pointer to any user data that will be passed to IrRecvCallback
	void * IrRecvCbContext;

	//! Enumerate devices
	//! DevList: List of detected devices
	//! Return: true - success, false - fail
	static bool EnumDevices(std::vector<std::string> &DevList);

	//! Convert NEC IR code to Tiqiaa signal data
	//! IrCode: Input code
	//! OutBuf: Buffer for signal data, >= 93 bytes
	//! Return: size of signal data
	static int WriteIrNecSignal(uint16_t IrCode, uint8_t * OutBuf);

	TiqiaaUsbIr();
	virtual ~TiqiaaUsbIr();

	//! Open device
	//! device_path: Path to device, can be obtained by EnumDevices()
	//! Return: true - success, false - fail
	bool Open(const char * device_path);

	//! Close device
	//! Return: true - success, false - fail
	bool Close();

	//! Return: true - device is open
	bool IsOpen();

	//! Send command to device and return immideately
	//! cmdType: Command type, one of Cmd* constant
	//! cmdId: Command ID, can be obtained by GetCmdId()
	//! Return: true - success, false - fail
	bool SendCmd(uint8_t cmdType, uint8_t cmdId);

	//! Send IR data to device and return immideately
	//! freq: Carrier freq - 0..255 - direct freq ID (index of TiqiaaUsbIr_IrFreqTable), one of TiqiaaUsbIr_IrFreqTable values - freq in HZ
	//! buffer: IR signal data
	//! buf_size: size of buffer
	//! cmdId: Command ID, can be obtained by GetCmdId()
	//! Return: true - success, false - fail
	//! Note: This function will not check device mode
	bool SendIRCmd(int freq, void * buffer, int buf_size, uint8_t cmdId);

	//! Send command to device and wait for completion
	//! cmdType: Command type, one of Cmd* constant
	//! cmdId: Command ID, can be obtained by GetCmdId()
	//! timeout: Timeout for waiting, msec
	//! Return: true - success, false - fail
	bool SendCmdAndWaitReply(uint8_t cmdType, uint8_t cmdId, DWORD timeout);

	//! Start waiting for command reply
	//! cmdType: Command type, one of Cmd* constant
	//! cmdId: Command ID, can be obtained by GetCmdId()
	//! Return: true - success, false - fail
	bool StartCmdReplyWaiting(uint8_t cmdType, uint8_t cmdId);

	//! Wait for command reply
	//! timeout: Timeout for waiting, msec
	//! Return: true - reply was received, false - fail or timeout expired
	bool WaitCmdReply(DWORD timeout);

	//! Cancel waiting for command reply
	//! Return: true - success, false - fail
	bool CancelCmdReplyWaiting();

	//! Get command ID for next command
	//! Return: Command ID
	uint8_t GetCmdId();

	//! Switch device to Idle mode
	//! Return: true - success, false - fail
	bool SetIdleMode();

	//! Send IR data to device and wait for completion
	//! freq: Carrier freq - 0..255 - direct freq ID, one of TiqiaaUsbIr_IrFreqTable values - freq in HZ
	//! buffer: IR signal data
	//! buf_size: size of buffer
	//! Return: true - success, false - fail
	//! Note: This function will switch device to Send mode
	bool SendIR(int freq, void * buffer, int buf_size);

	//! Start receiving of IR signal
	//! Return: true - success, false - fail
	//! Note: This function will switch device to Recv mode;
	//! After signal receive IrRecvCallback will be called;
	//! This function should be called again to receive next IR signal;
	//! This function should not be called from IrRecvCallback, call SendCmd(CmdOutput) instead
	//! Receive can be aborted by calling SetIdleMode, SendIR, SendNecSignal, SendCmd(CmdCancel)
	bool StartRecvIR();

	//! Send NEC IR code signal and wait for completion
	//! IrCode: NEC IR code
	//! Return: true - success, false - fail
	//! Note: This function will switch device to Send mode
	bool SendNecSignal(uint16_t IrCode);

	private:
	static DWORD WINAPI RunReadThreadFn(TiqiaaUsbIr * cls);
	static void WriteIrNecSignalPulse(TqIrWriteData * IrWrData, int PulseCount, bool isSet);

	bool SendReport2(void * data, int size);
	void ProcessRecvPacket(uint8_t * data, int size);
	void ReadThreadFn();
};

#endif
