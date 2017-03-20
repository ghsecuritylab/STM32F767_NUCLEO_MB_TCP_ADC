#include <stdio.h>
#include <socket.h>
#include <string.h>

#include "port.h"

/* ----------------------- Modbus includes ----------------------------------*/
#include "mb.h"
#include "mbport.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"


SOCKET          xListenSocket;
static fd_set   allset;



stMB_TCPClient *Current_MB_TCPClient;

#define MB_TCP_CLIENT_NUM					2
#define MB_TCP_CLIENT_STACK_SIZE	256

stMB_TCPClient MB_TCPClient[MB_TCP_CLIENT_NUM]={{INVALID_SOCKET,{0},0,0} , {INVALID_SOCKET,{0},0,0}};

SemaphoreHandle_t xMB_FrameRec_Mutex;

/* ----------------------- Static functions ---------------------------------*/
BOOL            prvMBTCPPortAddressToString( SOCKET xSocket, CHAR * szAddr, USHORT usBufSize );
CHAR           *prvMBTCPPortFrameToString( UCHAR * pucFrame, USHORT usFrameLen );
static BOOL     prvbMBPortAcceptClient( stMB_TCPClient *MB_TCPClient );
static void     prvvMBPortReleaseClient( stMB_TCPClient *MB_TCPClient);
static void 		usleep(uint32_t time);
void xMBTCPPort_HandlingTask( void *pvParameters );


/* ----------------------- Begin implementation -----------------------------*/

static void 		usleep(uint32_t time)
{
		while(time)
		{
				time--;
		}
}


BOOL
xMBTCPPortInit( USHORT usTCPPort )
{
    USHORT          usPort;
    struct sockaddr_in serveraddr;
	
		 xMB_FrameRec_Mutex = xSemaphoreCreateMutex();

    if( usTCPPort == 0 )
    {
        usPort = MB_TCP_DEFAULT_PORT;
    }
    else
    {
        usPort = ( USHORT ) usTCPPort;
    }
    memset( &serveraddr, 0, sizeof( serveraddr ) );
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl( INADDR_ANY );
    serveraddr.sin_port = htons( usPort );
    if( ( xListenSocket = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP ) ) == -1 )
    {
        fprintf( stderr, "Create socket failed.\r\n" );
        return FALSE;
    }
    else if( bind( xListenSocket, ( struct sockaddr * )&serveraddr, sizeof( serveraddr ) ) == -1 )
    {
        fprintf( stderr, "Bind socket failed.\r\n" );
        return FALSE;
    }
    else if( listen( xListenSocket, 5 ) == -1 )
    {
        fprintf( stderr, "Listen socket failed.\r\n" );
        return FALSE;
    }
    FD_ZERO( &allset );
    FD_SET( xListenSocket, &allset );
		
		listen(xListenSocket,2);
    return TRUE;
}

//void
//vMBTCPPortClose(  )
//{
//    // Close all client sockets. 
//    if( xClientSocket != SOCKET_ERROR )
//    {
//        prvvMBPortReleaseClient(  );
//    }
//    // Close the listener socket.
//    if( xListenSocket != SOCKET_ERROR )
//    {
//        close( xListenSocket );
//    }
//}

void
vMBTCPPortDisable( void )
{
    /* Close all client sockets. */
    if( Current_MB_TCPClient->xClientSocket != SOCKET_ERROR )
    {
        prvvMBPortReleaseClient( Current_MB_TCPClient );
    }
}

BOOL
xMBPortTCPPool( void )
{
		int	clnt_index=0;

		for(clnt_index=0;clnt_index<MB_TCP_CLIENT_NUM;clnt_index++)
		{
			if( MB_TCPClient[clnt_index].xClientSocket == INVALID_SOCKET )
			{
//					//listen(xListenSocket,2);
//					/* Accept to client */
//					select( xListenSocket + 1, &allset, NULL, NULL, NULL );
//					if( FD_ISSET( xListenSocket, &allset ) )
//					{
							if(prvbMBPortAcceptClient( &MB_TCPClient[clnt_index] )==TRUE)
							{
								  xTaskCreate( xMBTCPPort_HandlingTask, "MBTCP HANDLE", MB_TCP_CLIENT_STACK_SIZE, (void*)&MB_TCPClient[clnt_index], 3, NULL );
							}
//					}
			}
		}	
    return TRUE;
}


BOOL
xMBTCPPortGetRequest( UCHAR ** ppucMBTCPFrame, USHORT * usTCPLength )
{
    *ppucMBTCPFrame = &Current_MB_TCPClient->aucTCPBuf[0];
    *usTCPLength = Current_MB_TCPClient->usTCPBufPos;

    /* Reset the buffer. */
    Current_MB_TCPClient->usTCPBufPos = 0;
    Current_MB_TCPClient->usTCPFrameBytesLeft = MB_TCP_FUNC;
    return TRUE;
}


uint8_t activeSocket=0;

BOOL
xMBTCPPortSendResponse( const UCHAR * pucMBTCPFrame, USHORT usTCPLength )
{
    BOOL            bFrameSent = FALSE;
    BOOL            bAbort = FALSE;
    int             res;
    int             iBytesSent = 0;
    int             iTimeOut = MB_TCP_READ_TIMEOUT;

    do
    {
        res = send( Current_MB_TCPClient->xClientSocket, &pucMBTCPFrame[iBytesSent], usTCPLength - iBytesSent, 0 );
        switch ( res )
        {
        case -1:
            if( iTimeOut > 0 )
            {
                iTimeOut -= MB_TCP_READ_CYCLE;
                usleep( MB_TCP_READ_CYCLE );
            }
            else
            {
                bAbort = TRUE;
            }
            break;
        case 0:
            prvvMBPortReleaseClient(Current_MB_TCPClient );
            bAbort = TRUE;
            break;
        default:
            iBytesSent += res;
            break;
        }
    }
    while( ( iBytesSent != usTCPLength ) && !bAbort );

    bFrameSent = iBytesSent == usTCPLength ? TRUE : FALSE;
		
		xSemaphoreGive( xMB_FrameRec_Mutex );
    return bFrameSent;
}

void
prvvMBPortReleaseClient( stMB_TCPClient *MB_TCPClient )
{
    ( void )recv( MB_TCPClient->xClientSocket, &MB_TCPClient->aucTCPBuf[0], MB_TCP_BUF_SIZE, 0 );

    ( void )close( MB_TCPClient->xClientSocket );
    MB_TCPClient->xClientSocket = INVALID_SOCKET;
}

BOOL
prvbMBPortAcceptClient( stMB_TCPClient *MB_TCPClient )
{
    SOCKET          xNewSocket;
    BOOL            bOkay;

    /* Check if we can handle a new connection. */

    if( MB_TCPClient->xClientSocket != INVALID_SOCKET )
    {
        fprintf( stderr, "can't accept new client. all connections in use.\n" );
        bOkay = FALSE;
    }
    else if( ( xNewSocket = accept( xListenSocket, NULL, NULL ) ) == INVALID_SOCKET )
    {
        bOkay = FALSE;
    }
    else
    {
        MB_TCPClient->xClientSocket = xNewSocket;
        MB_TCPClient->usTCPBufPos = 0;
        MB_TCPClient->usTCPFrameBytesLeft = MB_TCP_FUNC;
        bOkay = TRUE;
    }
    return bOkay;
}


void xMBTCPPort_HandlingTask( void *pvParameters )
{
		fd_set          fread;
		int             ret;
		USHORT          usLength;
	  struct timeval  tval;

    tval.tv_sec = 0;
    tval.tv_usec = 5000;
	
		stMB_TCPClient *MB_TCPClient;
		MB_TCPClient=(stMB_TCPClient*)pvParameters;
	
	  while(1)
    {
        FD_ZERO( &fread );
        FD_SET( MB_TCPClient->xClientSocket, &fread );
			
			  ret = select( MB_TCPClient->xClientSocket + 1, &fread, NULL, NULL, &tval );
        if(( ret == SOCKET_ERROR ) || (!ret) )
        {
            continue;
        }
				
				
        if( ret > 0 )
        {
            if( FD_ISSET( MB_TCPClient->xClientSocket, &fread ) )
            {
								ret = recv( MB_TCPClient->xClientSocket, &MB_TCPClient->aucTCPBuf[MB_TCPClient->usTCPBufPos], MB_TCPClient->usTCPFrameBytesLeft,0 );
                if(( ret == SOCKET_ERROR ) || ( !ret ) )
                {
                    close( MB_TCPClient->xClientSocket );
                    MB_TCPClient->xClientSocket = INVALID_SOCKET;
                    vTaskDelete(NULL);
                }
								
                MB_TCPClient->usTCPBufPos += ret;
                MB_TCPClient->usTCPFrameBytesLeft -= ret;
								
                if( MB_TCPClient->usTCPBufPos >= MB_TCP_FUNC )
                {
                    /* Length is a byte count of Modbus PDU (function code + data) and the
                     * unit identifier. */
                    usLength = MB_TCPClient->aucTCPBuf[MB_TCP_LEN] << 8U;
                    usLength |= MB_TCPClient->aucTCPBuf[MB_TCP_LEN + 1];

                    /* Is the frame already complete. */
                    if( MB_TCPClient->usTCPBufPos < ( MB_TCP_UID + usLength ) )
                    {
                        MB_TCPClient->usTCPFrameBytesLeft = usLength + MB_TCP_UID - MB_TCPClient->usTCPBufPos;
                    }
                    /* The frame is complete. */
                    else if( MB_TCPClient->usTCPBufPos == ( MB_TCP_UID + usLength ) )
                    {

											
												if(1)// xSemaphoreTake( xMB_FrameRec_Mutex, ( TickType_t ) 100 ) == pdTRUE )
												{
														( void )xMBPortEventPost( EV_FRAME_RECEIVED );
														Current_MB_TCPClient=MB_TCPClient;													
												}
												else
												{
														close( MB_TCPClient->xClientSocket );
														MB_TCPClient->xClientSocket = INVALID_SOCKET;
														vTaskDelete(NULL);
												}
                    }
                    /* This can not happend because we always calculate the number of bytes
                     * to receive. */
                    else
                    {
                        assert( MB_TCPClient->usTCPBufPos <= ( MB_TCP_UID + usLength ) );
                    }
                }
            }
        }
    }
	
		vTaskDelete(NULL);
}