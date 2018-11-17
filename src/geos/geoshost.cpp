#include "dosbox.h"



#ifdef C_GEOSHOST

#include "mem.h"
#include "bios.h"
#include "regs.h"
#include "cpu.h"
#include "callback.h"
#include "inout.h"
#include "pic.h"
#include "hardware.h"
#include "joystick.h"
#include "mouse.h"
#include "setup.h"
#include "serialport.h"
#include "cpu.h"


//#include <android/log.h>
#include "SDL_net.h"
#include "SDL.h"


struct SocketState {
	bool used;
	bool open;
	bool blocking;
	TCPsocket socket;
	SDLNet_SocketSet socketSet;
	char* recvBuf;
	int recvBufUsed;
	bool receiveDone;
	bool done;

	SocketState() : used(false), recvBuf(NULL),recvBufUsed(0), receiveDone(false), done(false) {}
};

static const int MaxSockets = 256;

static SocketState NetSockets[MaxSockets];


static Bitu call_geoshost;
static bool G_callbackPending = false;
static bool G_receiveCallbackInit = false;
static Bit16u G_receiveCallbackSeg;
static Bit16u G_receiveCallbackOff;

static RealPt G_retrievalCallback;
static Bitu G_callRetrieval;
static bool G_receiveCallActive = false;

enum GeosHostCommands {
	GHC_CHECK = 1,
	GHC_SET_RECEIVE_HANDLE = 2,
	GHC_NETWORKING_BASE = 1000,
	GHC_NC_RESOLVE_ADDR = GHC_NETWORKING_BASE,
	GHC_NC_ALLOC_CONNECTION = GHC_NETWORKING_BASE + 1,
	GHC_NC_CONNECT_REQUEST = GHC_NETWORKING_BASE + 2,
	GHC_NC_SEND_DATA = GHC_NETWORKING_BASE + 3,
	GHC_NC_NEXT_RECV_SIZE = GHC_NETWORKING_BASE + 4,
	GHC_NC_RECV_NEXT = GHC_NETWORKING_BASE + 5,
	GHC_NC_RECV_NEXT_CLOSED = GHC_NETWORKING_BASE + 6,
	GHC_NC_CLOSE = GHC_NETWORKING_BASE + 7,
	GHC_NETWORKING_END = 1200
};

enum NetworkingCommands {

	NC_OPEN,
	NC_OPEN_NON_BLOCKING,
	NC_CLOSE,
	NC_SEND,
	NC_RECV,
};



enum GeosHostInterfaces {
	GHI_NETWORKING = 1
};

enum GeosHostError {
	GHE_SUCCESS,
	GHE_UNAVAILABLE_INTERFACE
};

static void NetStartReceiver(int handle);


static void NetSetReceiveHandle() {

	Bit16u pseg = SegValue(es);
	Bit16u pofs = reg_bx;

	if ((pseg == 0) && (pofs == 0)) {
		G_receiveCallbackInit = false;
	}
	else {
		G_receiveCallbackInit = true;
		G_receiveCallbackSeg = pseg;
		G_receiveCallbackOff = pofs;
	}

}


// GHC_CHECK
// Parameters:
// al = GHC_CHECK
// ds:si - pointer to null terminated string to connect to. Will get passed to DNS resolver.
// cx - interface
//			GHI_NETWORKING
// Return value:
// ax - error code (0 = success)
// bx - interface command base


static void GeosHostCheckInterface() {

	//__android_log_print(ANDROID_LOG_DEBUG, "GeosHost", "GeosHostCheckInterface");

	switch (reg_cx) {

	case GHI_NETWORKING:
		reg_bx = GHC_NETWORKING_BASE;
		reg_ax = GHE_SUCCESS;
		return;

	default:
		reg_ax = GHE_UNAVAILABLE_INTERFACE;
		return;
	}
}

static void NetResolveAddr() {

	char host[256];

	// TODO check buffer size

	MEM_StrCopy(SegPhys(ds) + reg_si, host, reg_cx); // 1024 toasts the stack
	host[reg_cx] = 0;
	LOG_MSG("NetResolveAddr %s", host);

	IPaddress ipaddress;
	int result = SDLNet_ResolveHost(&ipaddress, host, 1234);
	if (result == 0) {

		reg_dx = (ipaddress.host >> 16) & 0xFFFF;
		reg_ax = ipaddress.host & 0xFFFF;
		reg_flags &= ~FLAG_CF;

		LOG_MSG("NetResolveAddr success %x", ipaddress.host);
		return;
	}

	reg_flags |= FLAG_CF;
}

static void NetAllocConnection() {

	int socketHandle = -1;

	//__android_log_print(ANDROID_LOG_DEBUG, "GeosHost", "NetAllocConnection");

	for (int i = 1; i < MaxSockets; i++) {
		if (!NetSockets[i].used) {
			socketHandle = i;
			break;
		}
	}

	if (socketHandle < 0) { // no free sockets
		LOG_MSG("ERROR No free sockets");
		reg_ax = -1;
		reg_flags |= FLAG_CF;
		return;
	}

	LOG_MSG("Opening socket handle %d\n", socketHandle);
	SocketState &sock = NetSockets[socketHandle];

	sock.used = true;
	sock.open = false;
	sock.blocking = false;

	reg_ax = socketHandle;
	reg_flags |= FLAG_CF;
}

// GHC_NC_CONNECT_REQUEST
// Parameters:
// al = GHC_NC_CONNECT_REQUEST
// ds:si - ip addr to connect to
// cx - interface
// bx - socket handle
// dx - remote port
//			GHI_NETWORKING
// Return value:
// ax - error code (0 = success)
// bx - interface command base

static void NetConnectRequest() {

	LOG_MSG("NetConnectRequest port: %u  sock: %u  addr: %u.%u.%u.%u", reg_dx, reg_bx,
			reg_si & 0xFF,
			(reg_si >> 8) & 0xFF,
			reg_di & 0xFF,
			(reg_di >> 8) & 0xFF
	);

	IPaddress ip;
	ip.host = ((((int)reg_di) << 16) & 0xFFFF0000) | (reg_si & 0xFFFF);
	ip.port = ((reg_dx & 0xFF) << 8) | ((reg_dx >> 8) & 0xFF);

	LOG_MSG("NetConnectRequest Socket handle: %d", reg_bx);

	SocketState &sock = NetSockets[reg_bx];

	if (!(sock.socket = SDLNet_TCP_Open(&ip))) {
		//__android_log_print(ANDROID_LOG_DEBUG, "GeosHost", "TCP Open failed %x\n", ip.host);
		//if (!sock.blocking) {
		//	SDLNet_FreeSocketSet(sock.socketSet);
		//}
		reg_ax = -3;
		LOG_MSG("NetConnectRequest failed");
		return;
	}

	//SDLNet_TCP_AddSocket(sock.socketSet, sock.socket);

	//__android_log_print(ANDROID_LOG_DEBUG, "GeosHost", "Socket opened!\n");

	sock.open = true;

	// connected, for testing start receive thread now
	NetStartReceiver(reg_bx);
	//PIC_ActivateIRQ(5);
}

static int ReceiveThread(void* sockPtr)
{
	LOG_MSG("\nReceive thread started...");

	SocketState* sock = (SocketState*)sockPtr;
	do {
		// wait for data buffer to be supplied by dos request, or cancelled

		if (sock->recvBufUsed <= 0) {

			LOG_MSG("\nReceived get");
			if (sock->recvBuf == NULL) {
				LOG_MSG("\nReceived new buf");
				sock->recvBuf = new char[512];
			}

			int result = SDLNet_TCP_Recv(((SocketState*)sock)->socket, sock->recvBuf, 512);
			LOG_MSG("\nReceived data %d", result);
			if ((!sock->done) && (result > 0)) {

				// pass data to DOS
				sock->recvBufUsed = result;
				G_callbackPending = true;
				//PIC_ActivateIRQ(5);
			}
			else {

				// handle receive error
				LOG_MSG("\nReceived done");
				SDL_Delay(5000);
				((SocketState*)sock)->receiveDone = true;
				return 0;
			}
		}
		else {

			// pending, wait some time and retry
			SDL_Delay(50);
		}


	} while (true);
}


static void NetStartReceiver(int handle) {

	// start receiver thread
	SDL_Thread *thread;
	int         threadReturnValue;

	LOG_MSG("\nSimple SDL_CreateThread test:");

	SocketState &sock = NetSockets[handle];

	// Simply create a thread
	thread = SDL_CreateThread(ReceiveThread, (void *)&sock);

	if (NULL == thread) {
		LOG_MSG("\nSDL_CreateThread failed: %s\n", SDL_GetError());
	}
	else {
		//SDL_WaitThread(thread, &threadReturnValue);
		//printf("\nThread returned value: %d", threadReturnValue);
	}
}


// GHC_NC_SEND_DATA
// Parameters:
// al = GHC_NC_SEND_DATA
// es:si - ip addr to connect to
// cx - data size
// bx - socket handle
// Return value:
// ax - error code (0 = success)
// bx - interface command base

static void NetSendData() {

	//__android_log_print(ANDROID_LOG_DEBUG, "GeosHost", "NetSendData");

	int socketHandle = reg_bx;
	LOG_MSG("NetSendData Socket handle: %d", socketHandle);

	if (socketHandle < 0 || socketHandle >= MaxSockets) {
		reg_ax = -1;
		return;
	}
	SocketState &sock = NetSockets[socketHandle];

	PhysPt dosBuff = SegPhys(es) + reg_si;
	int size = reg_cx;
	LOG_MSG("NetSendData data size: %d", size);

	char *buffer = new char[size + 1];
	for (int i = 0; i < size; i++) {
		buffer[i] = mem_readb(dosBuff + i);
	}
	buffer[size] = 0;
	LOG_MSG("Sending %d bytes: %s\n", size, buffer);


	int sockhandle = reg_bx;
	int sent = SDLNet_TCP_Send(sock.socket, buffer, size);
	if (sent < size) {

		LOG_MSG("NetSendData send failed: %d %d", sent, socketHandle);
	}
	else {

		LOG_MSG("NetSendData send success");
	}
	reg_ax = 0;

	delete[] buffer;
}

static void NetNextRecvSize() {

	//PIC_DeActivateIRQ(11);

	// init, return value with no bug pending
	reg_cx = 0;

	for (int i = 0; i < MaxSockets; i++) {
		
		if (NetSockets[i].used) {
			
			if (!NetSockets[i].done) {
				// check if received data pending
				if (NetSockets[i].recvBufUsed > 0) {

					// return size;
					reg_cx = NetSockets[i].recvBufUsed;
					break;
				}
			}
		}
	}
	if (reg_cx > 0) {
		LOG_MSG("NetNextRecvSize: %d", reg_cx);
	}
}

static void NetClose() {

	LOG_MSG("NetCLOSE: %d", reg_cx);
	SocketState &sock = NetSockets[reg_cx];

	if (sock.used) {

		sock.done = true;
		//SDLNet_TCP_Close(sock.socket);
	}
}


static void NetRecvNextClosed() {

	for (int i = 0; i < MaxSockets; i++) {

		if (NetSockets[i].used) {

			if (!NetSockets[i].done) {
				if (NetSockets[i].receiveDone) {

					NetSockets[i].receiveDone = false;
					reg_cx = i;
					if (reg_cx > 0) {
						LOG_MSG("NetRecvNextClosed: %d", reg_cx);
					}
					return;
				}
			}
		}
	}
	reg_cx = 0;
}

static void NetRecvNext() {

	// find available socket with 
	for (int i = 0; i < MaxSockets; i++) {

		if (NetSockets[i].used) {

			if (!NetSockets[i].done) {
				// check if received data pending
				if (NetSockets[i].recvBufUsed == reg_cx) {

					// found buffer of right size
					// copy it
					PhysPt dosBuff = SegPhys(es) + reg_di;
					int size = reg_cx;


					LOG_MSG("RECEIVENEXT: %x %x", SegPhys(es), reg_di);
					for (int i2 = 0; i2 < size; i2++) {
						mem_writeb(dosBuff + i2, NetSockets[i].recvBuf[i2]);
					}

					// mark unused, so continue receiving
					NetSockets[i].recvBufUsed = 0;

					reg_dx = i;
					break;
				}
			}
		}
	}
}


static Bitu INTB0_Handler(void) {

	if (!SDLNetInited) {
		SDLNet_Init();
		SDLNetInited = true;
	}

	if (reg_ax == GHC_CHECK) {

		GeosHostCheckInterface();
	}
	else if (reg_ax == GHC_SET_RECEIVE_HANDLE) {

		NetSetReceiveHandle();
	}
	else if (reg_ax == GHC_NC_RESOLVE_ADDR) {

		NetResolveAddr();
	}
	else if (reg_ax == GHC_NC_ALLOC_CONNECTION) {

		NetAllocConnection();
	}
	else if (reg_ax == GHC_NC_CONNECT_REQUEST) {

		NetConnectRequest();
	}
	else if (reg_ax == GHC_NC_SEND_DATA) {

		NetSendData();
	}
	else if (reg_ax == GHC_NC_NEXT_RECV_SIZE) {

		NetNextRecvSize();
	}
	else if (reg_ax == GHC_NC_RECV_NEXT) {

		NetRecvNext();
	}
	else if (reg_ax == GHC_NC_RECV_NEXT_CLOSED) {

		NetRecvNextClosed();
	}
	else if (reg_ax == GHC_NC_CLOSE) {

		NetClose();
	}
	else if ((reg_ax >= GHC_NETWORKING_BASE) && (reg_ax < GHC_NETWORKING_END)) {


	}

	switch (reg_ax) {
	case GHC_CHECK:
		break;
		/*case NC_OPEN:
		NetOpen(true);
		break;
		case NC_OPEN_NON_BLOCKING:
		NetOpen(false);
		break;
		case NC_CLOSE:
		NetClose();
		break;
		case NC_SEND:
		NetSend();
		break;
		case NC_RECV:
		NetRecv();
		break;*/
	}
	return CBRET_NONE;
}

static Bitu RetrievalCallback_Handler(void) {
	LOG_MSG("RetrievalCallback_Handler");
	G_receiveCallActive = false;
	return CBRET_NONE;
}


static void CALLBACK_Poller(void) {

	if (G_callbackPending) {

		if (G_receiveCallbackInit && !G_receiveCallActive) {
			LOG_MSG("CALLBACK_Poller");

			G_receiveCallActive = true;
			//CALLBACK_RunRealFar(G_receiveCallbackSeg, G_receiveCallbackOff);
			G_callbackPending = false;
			G_receiveCallActive = false;
			LOG_MSG("CALLBACK_Poller2");
		}
	}
}


void GeosHost_Init(Section* /*sec*/) {

	memset(NetSockets, 0, sizeof(SocketState)*MaxSockets);

	G_callRetrieval = CALLBACK_Allocate();
	CALLBACK_Setup(G_callRetrieval, &RetrievalCallback_Handler, CB_RETF, "retrieval callback");
	G_retrievalCallback = CALLBACK_RealPointer(G_callRetrieval);


	//TIMER_AddTickHandler(CALLBACK_Poller);

	/* Setup the INT B0 vector */
	call_geoshost = CALLBACK_Allocate();
	CALLBACK_Setup(call_geoshost, &INTB0_Handler, CB_IRET, "Geoshost");
	RealSetVec(0xB0, CALLBACK_RealPointer(call_geoshost));
	//__android_log_print(ANDROID_LOG_DEBUG, "GeosHost", "GeosHost_Init");
	LOG_MSG("Geoshost initialized\n");
}


#endif // C_GEOSHOST



