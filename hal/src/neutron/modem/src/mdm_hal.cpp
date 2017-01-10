/**
 ******************************************************************************
 Copyright (c) 2013-2014 IntoRobot Team.  All right reserved.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation, either
 version 3 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

/* Includes -----------------------------------------------------------------*/
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "hw_config.h"
#include "mdm_hal.h"
#include "timer_hal.h"
#include "delay_hal.h"
#include "gpio_hal.h"
#include "service_debug.h"
#include "concurrent_hal.h"
#include "net_hal.h"
#include "crc16.h"
#include "flash_map.h"
#include "flash_storage_impl.h"


#ifdef putc
#undef putc
#undef getc
#endif

extern "C"
{
    static void __modem_lock(void);
    static void __modem_unlock(void);
}

/* Private typedef ----------------------------------------------------------*/

/* Private define -----------------------------------------------------------*/
/* Private macro ------------------------------------------------------------*/

#define PROFILE         "0"   //!< this is the psd profile used
#define MAX_SIZE        1024  //!< max expected messages (used with RX)
#define USO_MAX_WRITE   1024  //!< maximum number of bytes to write to socket (used with TX)
// num sockets
#define NUMSOCKETS      ((int)(sizeof(_sockets)/sizeof(*_sockets)))
//! test if it is a socket is ok to use
#define ISSOCKET(s)     (((s) >= 0) && ((s) < NUMSOCKETS) && (_sockets[s].handle != MDM_SOCKET_ERROR))
//! check for timeout
#define TIMEOUT(t, ms)  ((ms != TIMEOUT_BLOCKING) && ((HAL_Timer_Get_Milli_Seconds() - t) > ms))
//! helper to make sure that lock unlock pair is always balanced
#define LOCK()      __modem_lock()
//! helper to make sure that lock unlock pair is always balanced
#define UNLOCK()    __modem_unlock()


static os_mutex_recursive_t modem_mutex = 0;

void init_modem_mutex(void)
{
    os_mutex_recursive_create(&modem_mutex);
}

static void __modem_lock(void)
{
    if (modem_mutex)
        os_mutex_recursive_lock(modem_mutex);
}

static void __modem_unlock(void)
{
    if (modem_mutex)
        os_mutex_recursive_unlock(modem_mutex);
}

#ifdef MDM_DEBUG
#if 0 // colored terminal output using ANSI escape sequences
#define COL(c) "\033[" c
#else
#define COL(c) ""
#endif
#define DEF COL("39m")
#define BLA COL("30m")
#define RED COL("31m")
#define GRE COL("32m")
#define YEL COL("33m")
#define BLU COL("34m")
#define MAG COL("35m")
#define CYA COL("36m")
#define WHY COL("37m")

void dumpAtCmd(const char* buf, int len)
{
    DEBUG_D(" %3d \"", len);
    while (len --) {
        char ch = *buf++;
        if ((ch > 0x1F) && (ch < 0x7F)) { // is printable
            if      (ch == '%')  DEBUG_D("%%");
            else if (ch == '"')  DEBUG_D("\\\"");
            else if (ch == '\\') DEBUG_D("\\\\");
            else DEBUG_D("%c", ch);
        } else {
            if      (ch == '\a') DEBUG_D("\\a"); // BEL (0x07)
            else if (ch == '\b') DEBUG_D("\\b"); // Backspace (0x08)
            else if (ch == '\t') DEBUG_D("\\t"); // Horizontal Tab (0x09)
            else if (ch == '\n') DEBUG_D("\\n"); // Linefeed (0x0A)
            else if (ch == '\v') DEBUG_D("\\v"); // Vertical Tab (0x0B)
            else if (ch == '\f') DEBUG_D("\\f"); // Formfeed (0x0C)
            else if (ch == '\r') DEBUG_D("\\r"); // Carriage Return (0x0D)
            else                 DEBUG_D("\\x%02x", (unsigned char)ch);
        }
    }
    DEBUG_D("\"\r\n");
}

void MDMParser::_debugPrint(int level, const char* color, const char* format, ...)
{
    if (_debugLevel >= level)
    {
        va_list args;
        va_start (args, format);
        if (color) DEBUG_D(color);
        DEBUG_D(format, args);
        if (color) DEBUG_D(DEF);
        va_end (args);
        DEBUG_D("\r\n");
    }
}
// Warning: Do not use these for anything other than constant char messages,
// they will yield incorrect values for integers.  Use DEBUG_D() instead.
#define MDM_ERROR(...)  do {_debugPrint(0, RED, __VA_ARGS__);}while(0)
#define MDM_INFO(...)   do {_debugPrint(1, GRE, __VA_ARGS__);}while(0)
#define MDM_TRACE(...)  do {_debugPrint(2, DEF, __VA_ARGS__);}while(0)
#define MDM_TEST(...)   do {_debugPrint(3, CYA, __VA_ARGS__);}while(0)

#else

#define MDM_ERROR(...) // no tracing
#define MDM_TEST(...)  // no tracing
#define MDM_INFO(...)  // no tracing
#define MDM_TRACE(...) // no tracing

#endif

/* Private variables --------------------------------------------------------*/

MDMParser* MDMParser::inst;

/* Extern variables ---------------------------------------------------------*/

/* Private function prototypes ----------------------------------------------*/

MDMParser::MDMParser(void)
{
    inst = this;

    _smartconfig_status = DEALSTATUS_IDLE;
    _downotafile_status = DEALSTATUS_IDLE;
    _downnetfile_status = DEALSTATUS_IDLE;

    _init      = false;

    _aplisttotalcount  = 0;
    _aplistindex = 0;

    _cancel_all_operations = false;
    memset(_sockets, 0, sizeof(_sockets));
    for (int socket = 0; socket < NUMSOCKETS; socket ++)
        _sockets[socket].handle = MDM_SOCKET_ERROR;
#ifdef MDM_DEBUG
    _debugLevel = 3;
    _debugTime = HAL_Timer_Get_Milli_Seconds();
#endif
}

void MDMParser::cancel(void) {
    MDM_INFO("\r\n[ Modem::cancel ] = = = = = = = = = = = = = = =");
    _cancel_all_operations = true;
}

void MDMParser::resume(void) {
    MDM_INFO("\r\n[ Modem::resume ] = = = = = = = = = = = = = = =");
    _cancel_all_operations = false;
}

int MDMParser::send(const char* buf, int len)
{
#ifdef MDM_DEBUG
    if (_debugLevel >= 3) {
        DEBUG_D("%10.3f AT send    ", (HAL_Timer_Get_Milli_Seconds()-_debugTime)*0.001);
        dumpAtCmd(buf,len);
    }
#endif
    return _send(buf, len);
}

int MDMParser::sendFormated(const char* format, ...) {
    if (_cancel_all_operations) return 0;

    char buf[MAX_SIZE];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf,sizeof(buf), format, args);
    va_end(args);
    return send(buf, len);
}

int MDMParser::waitFinalResp(_CALLBACKPTR cb /* = NULL*/,
        void* param /* = NULL*/,
        system_tick_t timeout_ms /*= 5000*/)
{
    if (_cancel_all_operations) return WAIT;

    char buf[MAX_SIZE + 64 /* add some more space for framing */];
    system_tick_t start = HAL_Timer_Get_Milli_Seconds();
    do {
        int ret = getLine(buf, sizeof(buf));
#ifdef MDM_DEBUG
        if ((_debugLevel >= 3) && (ret != WAIT) && (ret != NOT_FOUND))
        {
            int len = LENGTH(ret);
            int type = TYPE(ret);
            const char* s = (type == TYPE_UNKNOWN)? YEL "UNK" DEF :
                (type == TYPE_OK   )  ? GRE "OK " DEF :
                (type == TYPE_ERROR)  ? RED "ERR" DEF :
                (type == TYPE_ABORTED) ? RED "ABT" DEF :
                (type == TYPE_PLUS)   ? CYA " + " DEF :
                (type == TYPE_PROMPT) ? BLU " > " DEF :
                "..." ;
            DEBUG_D("%10.3f AT read %s", (HAL_Timer_Get_Milli_Seconds()-_debugTime)*0.001, s);
            dumpAtCmd(buf, len);
            (void)s;
        }
#endif
        if ((ret != WAIT) && (ret != NOT_FOUND))
        {
            int type = TYPE(ret);
            /*******************************************/
            //handle unsolicited commands here
            if (type == TYPE_PLUS) {
                const char* cmd = buf+1;
                int a, b, n;
                char *s;

                // Socket Specific Command ---------------------------------
                // +IPD, <socket>,<length>
                if ((sscanf(cmd, "IPD,%d,%d", &a, &b) == 2)) {
                    int socket = _findSocket(a);
                    DEBUG_D("Socket %d: handle %d has %d bytes pending\r\n", socket, a, b);
                    if (socket != MDM_SOCKET_ERROR) {
                        s = strchr(buf, ':');
                        for(n=0; n < b; n++) {
                            if (_sockets[socket].pipe->writeable()) {
                                _sockets[socket].pipe->putc(s[n+1]);
                            }
                            else{
                                break;
                            }
                        }
                        _sockets[socket].pending += n;
                    }
                    // down file ---------------------------------
                    // IR_DOWNFILE:<result>
                } else if ((sscanf(cmd, "IR_DOWNFILE:%d", &a) == 1)) {
                    //AT设计的有问题   下载中返回结果后面多了一个ok. 特殊处理  去掉后面的Ok
                    if(_downotafile_status == DEALSTATUS_DOING) {
                        char temp[16];
                        getLine(temp, strlen("\r\nOK\r\n"));
                    }
                    if(0 == a)
                        _downotafile_status = DEALSTATUS_SUCCESS;
                    else
                        _downotafile_status = DEALSTATUS_FAIL;
                    // IR_DOWNFILE:<result>
                } else if ((sscanf(cmd, "IR_NETDOWN:%d", &a) == 1)) {
                    //AT设计的有问题   下载中返回结果后面多了一个ok. 特殊处理  去掉后面的Ok
                    if(_downnetfile_status == DEALSTATUS_DOING) {
                        char temp[16];
                        getLine(temp, strlen("\r\nOK\r\n"));
                    }
                    if(0 == a)
                        _downnetfile_status = DEALSTATUS_SUCCESS;
                    else
                        _downnetfile_status = DEALSTATUS_FAIL;
                }
            } // end ==TYPE_PLUS
            else if (type == TYPE_SMARTCONFIG) {
                _smartconfig_status = DEALSTATUS_SUCCESS;
            } // end ==TYPE_PLUS
            /*******************************************/
            if (cb) {
                int len = LENGTH(ret);
                int ret = cb(type, buf, len, param);
                if (WAIT != ret)
                    return ret;
            }
            if (type == TYPE_OK)
                return RESP_OK;
            if (type == TYPE_FAIL)
                return RESP_FAIL;
            if (type == TYPE_ERROR)
                return RESP_ERROR;
            if (type == TYPE_PROMPT)
                return RESP_PROMPT;
            if (type == TYPE_ABORTED)
                return RESP_ABORTED; // This means the current command was ABORTED, so retry your command if critical.
        }
        // relax a bit
        HAL_Delay_Milliseconds(10);
    }
    while (!TIMEOUT(start, timeout_ms) && !_cancel_all_operations);

    return WAIT;
}

int MDMParser::_cbString(int type, const char* buf, int len, char* str)
{
    if (str && (type == TYPE_UNKNOWN)) {
        if (sscanf(buf, "\r\n%s\r\n", str) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int MDMParser::_cbInt(int type, const char* buf, int len, int* val)
{
    if (val && (type == TYPE_UNKNOWN)) {
        if (sscanf(buf, "\r\n%d\r\n", val) == 1)
            /*nothing*/;
    }
    return WAIT;
}

// ----------------------------------------------------------------
void MDMParser::reset(void)
{
    MDM_INFO("[ Modem reset ]");

    GPIO_InitTypeDef   GPIO_InitStruct;

    //esp8266 gpio1
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    //esp8266 reset
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
    HAL_Delay(200);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
}

bool MDMParser::init(void)
{
    init_modem_mutex();
    LOCK();
    if (!_init) {
        MDM_INFO("[ Esp8266 init ]");
        esp8266MDM.begin(460800);
        //reset();   //由于bootloader已经初始化过esp8266复位管教，可以不初始化，这样可以减少esp8266启动时间.
        /* Initialize only once */
        _init = true;
    }

    bool continue_cancel = false;
    bool retried_after_reset = false;

    int i = 10;
    while (i--) {
        if (_cancel_all_operations) {
            continue_cancel = true;
            resume(); // make sure we can talk to the modem
        }
        // echo off
        sendFormated("ATE0\r\n");
        int r = waitFinalResp(NULL,NULL,1000);
        if(RESP_OK == r) {
            break;
        }
        else if (i==0 && !retried_after_reset) {
            retried_after_reset = true; // only perform reset & retry sequence once
            i = 10;
            reset();
        }
    }

    if (i < 0) {
        MDM_ERROR("[ No Reply from Modem ]\r\n");
    }

    if (continue_cancel) {
        cancel();
        goto failure;
    }

    // enable mulit connect
    sendFormated("AT+CIPMUX=1\r\n");
    if(RESP_OK != waitFinalResp())
        goto failure;

    UNLOCK();
    return true;
failure:
    UNLOCK();
    return false;
}

int MDMParser::_cbGetNetVersion(int type, const char* buf, int len, char* str)
{
    if (str && (type == TYPE_PLUS)) {
        if (sscanf(buf, "+IR_GETVERSION:%s\r\n", str) == 1)
            /*nothing*/;
    }
    return WAIT;
}

bool MDMParser::getNetVersion(char *version)
{
    bool ok = false;
    LOCK();
    if (_init) {
        // get the net version
        sendFormated("AT+IR_GETVERSION?\r\n");
        if (RESP_OK == waitFinalResp(_cbGetNetVersion, version)){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

/*
 * 1: station  2:softAp  3:softAp + station
 */
bool MDMParser::setWifiMode(wifi_mode_t mode)
{
    bool ok = false;
    LOCK();
    if (_init) {
        sendFormated("AT+CWMODE_DEF=%d\r\n", mode);
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

bool MDMParser::setWifiDHCP(wifi_mode_t mode, char enable)
{
    bool ok = false;
    LOCK();
    if (_init) {
        sendFormated("AT+CWDHCP_DEF=%d,%d\r\n", mode ,enable);
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

bool MDMParser::startSmartconfig(smart_config_t type)
{
    bool ok = false;
    LOCK();
    if (_init) {
        sendFormated("AT+CWSTARTSMART=%d\r\n",type);
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    _smartconfig_status = DEALSTATUS_DOING;
    return ok;
}

bool MDMParser::stopSmartconfig(void)
{
    bool ok = false;
    LOCK();
    if (_init) {
        sendFormated("AT+CWSTOPSMART\r\n");
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    _smartconfig_status = DEALSTATUS_IDLE;
    return ok;
}

deal_status_t MDMParser::getSmartconfigStatus(void)
{
    waitFinalResp(NULL,NULL,0); //必须调用这个才能处理数据
    return _smartconfig_status;
}

int MDMParser::_cbGetAddress(int type, const char* buf, int len, wifi_addr_t* addr)
{
    if ((type == TYPE_PLUS) && addr) {
        int a,b,c,d;
        if (sscanf(buf, "+CIFSR:STAIP,\"" IPSTR "\"", &a,&b,&c,&d) == 4)
            addr->IpAddr = IPADR(a,b,c,d);
        uint8_t temp[6];
        if (sscanf(buf, "+CIFSR:STAMAC,\"" MACSTR "\"", &temp[0],&temp[1],&temp[2],&temp[3],&temp[4],&temp[5]) == 6)
            memcpy(addr->MacAddr, temp, 6);
    }
    return WAIT;
}

int MDMParser::getAddress(wifi_addr_t *addr)
{
    bool ok = false;
    LOCK();
    if (_init) {
        sendFormated("AT+CIFSR\r\n");
        if (RESP_OK == waitFinalResp(_cbGetAddress, addr)) {
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

bool MDMParser::setMacAddress(const char *staMac, const char *apMac)
{
    bool ok = false;
    LOCK();
    if (_init) {
        sendFormated("AT+CWMODE_CUR=3\r\n");
        if (RESP_OK != waitFinalResp())
            goto failure;
        // get the MSISDNs related to this subscriber
        sendFormated("AT+CIPSTAMAC_DEF=\"%s\"\r\n", staMac);
        if (RESP_OK != waitFinalResp())
            goto failure;

        // get the MSISDNs related to this subscriber
        sendFormated("AT+CIPAPMAC_DEF=\"%s\"\r\n", apMac);
        if (RESP_OK != waitFinalResp())
            goto failure;

        // get the MSISDNs related to this subscriber
        sendFormated("AT+CWMODE_CUR=1\r\n");
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
failure:
    UNLOCK();
    return false;
}

int MDMParser::_cbGetStaMacAddress(int type, const char* buf, int len, char* str)
{
    if (str && (type == TYPE_PLUS)) {
        if (sscanf(buf, "+CIPSTAMAC_DEF:%s\r\n", str) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int MDMParser::_cbGetApMacAddress(int type, const char* buf, int len, char* str)
{
    if (str && (type == TYPE_PLUS)) {
        if (sscanf(buf, "+CIPAPMAC_DEF:%s\r\n", str) == 1)
            /*nothing*/;
    }
    return WAIT;
}

bool MDMParser::getMacAddress(char *staMac, char *apMac)
{
    bool ok = false;
    LOCK();
    if (_init) {
        sendFormated("AT+CIPSTAMAC_DEF?\r\n");
        if (RESP_OK != waitFinalResp(_cbGetStaMacAddress, staMac))
            goto failure;

        sendFormated("AT+CIPAPMAC_DEF?\r\n");
        if (RESP_OK == waitFinalResp(_cbGetApMacAddress, apMac)){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
failure:
    UNLOCK();
    return false;
}

int MDMParser::_cbApScan(int type, const char* buf, int len, wifi_ap_t *aps)
{
#if 0
    if (aps && (type == TYPE_UNKNOWN)) {
        if (sscanf(buf, "\r\nSTATUS:%d\r\n", result) == 1)
            /*nothing*/;
    }
#endif
    return WAIT;
}


int MDMParser::apScan(wifi_ap_t* aps, size_t aps_count)
{
    int result = AT_ERROR;
    LOCK();

    _aplisttotalcount  = aps_count;
    _aplistindex = 0;

    if (_init) {
        sendFormated("AT+CWLAPOPT=1,31\r\n");
        if (RESP_OK != waitFinalResp())
            goto failure;

        sendFormated("AT+CWLAP\r\n");
        if (RESP_OK == waitFinalResp(_cbApScan, aps)){
            result = _aplistindex;
        }
    }
    UNLOCK();
    return result;
failure:
    UNLOCK();
    return AT_ERROR;
}

int MDMParser::_cbGetIpStatus(int type, const char* buf, int len, ip_status_t* result)
{
    int rst;
    if (result && (type == TYPE_UNKNOWN)) {
        if (sscanf(buf, "STATUS:%d\r\n", &rst) == 1) {
            *result = (ip_status_t)rst;
        }
    }
    return WAIT;
}

ip_status_t MDMParser::getIpStatus(void)
{
    ip_status_t result = IPSTATUS_ATERROR;
    LOCK();

    if (_init) {
        sendFormated("AT+CIPSTATUS\r\n");
        if (RESP_OK != waitFinalResp(_cbGetIpStatus, &result)){
            result = IPSTATUS_ATERROR;
        }
    }
    UNLOCK();
    return result;
}

int MDMParser::_cbGetWifiInfo(int type, const char* buf, int len, wifi_info_t *wifiInfo)
{
    int rst;
    if (wifiInfo && (type == TYPE_PLUS)) {
        if (sscanf(buf, "+CWJAP_DEF:\"%[^\"]\",\"" MACSTR "\",%d,%d\r\n", wifiInfo->ssid, \
                    &wifiInfo->bssid[0],  &wifiInfo->bssid[1], &wifiInfo->bssid[2], &wifiInfo->bssid[3], &wifiInfo->bssid[4], &wifiInfo->bssid[5],\
                    &wifiInfo->channel, &wifiInfo->rssi) == 9)
            /*nothing*/;
    }
    return WAIT;
}

int MDMParser::getWifiInfo(wifi_info_t *wifiInfo)
{
    bool ok = false;
    LOCK();
    if (_init) {
        memset(wifiInfo, 0, sizeof(wifi_info_t));
        sendFormated("AT+CWJAP_DEF?\r\n");
        if (RESP_OK == waitFinalResp(_cbGetWifiInfo, wifiInfo)) {
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

int MDMParser::_cbWifiJoinAp(int type, const char* buf, int len, wifi_join_ap_t* result)
{
    int rst;
    if (result && (type == TYPE_PLUS)) {
        if (sscanf(buf, "+CWJAP_DEF:%d\r\n", &rst) == 1) {
            *result = (wifi_join_ap_t)rst;
        }
            /*nothing*/;
    }
    return WAIT;
}

wifi_join_ap_t MDMParser::wifiJoinAp(const char *ssid, const char *password)
{
    wifi_join_ap_t result = JOINAP_CONNETFAIL;
    LOCK();

    if (_init) {
        sendFormated("AT+CWJAP_DEF=\"%s\",\"%s\"\r\n", ssid, password);
        if (WAIT == waitFinalResp(_cbWifiJoinAp, &result)) {
            result = JOINAP_CONNETFAIL;
        }
    }
    UNLOCK();
    return result;
}

wifi_join_ap_t MDMParser::wifiJoinAp(const char *ssid, const char *password, const char *bssid)
{
    wifi_join_ap_t result = JOINAP_CONNETFAIL;
    LOCK();

    if (_init) {
        sendFormated("AT+CWJAP_DEF=\"%s\",\"%s\",\"%s\"\r\n", ssid, password, bssid);
        if (WAIT == waitFinalResp(_cbWifiJoinAp, &result)){
            result = JOINAP_CONNETFAIL;
        }
    }
    UNLOCK();
    return result;
}

int MDMParser::_cbGetHostByName(int type, const char* buf, int len, MDM_IP* ip)
{
    if ((type == TYPE_PLUS) && ip) {
        int a,b,c,d;
        if (sscanf(buf, "+CIPDOMAIN:" IPSTR "\r\n", &a,&b,&c,&d) == 4)
            *ip = IPADR(a,b,c,d);
    }
    return WAIT;
}


MDM_IP MDMParser::getHostByName(const char* host)
{
    MDM_IP ip = NOIP;
    int a,b,c,d;
    if (sscanf(host, IPSTR, &a,&b,&c,&d) == 4)
        ip = IPADR(a,b,c,d);
    else {
        LOCK();
        sendFormated("AT+CIPDOMAIN=\"%s\"\r\n", host);
        if (RESP_OK != waitFinalResp(_cbGetHostByName, &ip, 30*1000))
            ip = NOIP;
        UNLOCK();
    }
    return ip;
}

bool MDMParser::ping(const char* host)
{
    bool ok = false;
    LOCK();

    if (_init) {
        sendFormated("AT+PING=\"%s\"\r\n", host);
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

bool MDMParser::ping(const MDM_IP& ip)
{
    bool ok = false;
    LOCK();

    if (_init) {
        sendFormated("AT+PING=\"" IPSTR "\"\r\n", IPNUM(ip));
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

int MDMParser::socketSocket(IpProtocol ipproto, int port)
{
    int socket;

    // find an free socket
    socket = _findSocket(MDM_SOCKET_ERROR);
    if (socket != MDM_SOCKET_ERROR) {
        DEBUG_D("Socket %d: handle %d was created\r\n", socket, socket);
        _sockets[socket].handle     = socket;
        _sockets[socket].ipproto    = ipproto;
        _sockets[socket].localip    = port;
        _sockets[socket].connected  = false;
        _sockets[socket].pending    = 0;
        _sockets[socket].open       = true;
        _sockets[socket].pipe = new Pipe<char>(MAX_SIZE);
    }
    DEBUG_D("socketSocket(%s)\r\n", (ipproto?"UDP":"TCP"));
    return socket;
}

bool MDMParser::socketConnect(int socket, const char * host, int port)
{
    bool ok = false;
    LOCK();
    if (ISSOCKET(socket) && (!_sockets[socket].connected)) {
        DEBUG_D("socketConnect(%d,port:%d)\r\n", socket,port);
        if(_sockets[socket].ipproto)
            sendFormated("AT+CIPSTART=%d,\"%s\",\"%s\",%d,%d,%d\r\n", _sockets[socket].handle, "UDP", host, port, _sockets[socket].localip, 2);
        else
            sendFormated("AT+CIPSTART=%d,\"%s\",\"%s\",%d\r\n", _sockets[socket].handle, "TCP", host, port);
        if (RESP_OK == waitFinalResp())
            ok = _sockets[socket].connected = true;
    }
    UNLOCK();
    return ok;
}

bool MDMParser::socketConnect(int socket, const MDM_IP& ip, int port)
{
    bool ok = false;
    LOCK();
    if (ISSOCKET(socket) && (!_sockets[socket].connected)) {
        DEBUG_D("socketConnect(%d,port:%d)\r\n", socket,port);
        if(_sockets[socket].ipproto)
            sendFormated("AT+CIPSTART=%d,\"%s\",\"" IPSTR "\",%d,%d,%d\r\n", _sockets[socket].handle, "UDP", IPNUM(ip), port, _sockets[socket].localip, 2);
        else
            sendFormated("AT+CIPSTART=%d,\"%s\",\"" IPSTR "\",%d\r\n", _sockets[socket].handle, "TCP", IPNUM(ip), port);
        if (RESP_OK == waitFinalResp())
            ok = _sockets[socket].connected = true;
    }
    UNLOCK();
    return ok;
}

bool MDMParser::socketIsConnected(int socket)
{
    bool ok = false;
    LOCK();
    ok = ISSOCKET(socket) && _sockets[socket].connected;
    //DEBUG_D("socketIsConnected(%d) %s\r\n", socket, ok?"yes":"no");
    UNLOCK();
    return ok;
}

bool MDMParser::socketClose(int socket)
{
    bool ok = false;
    LOCK();
    if (ISSOCKET(socket)
            && (_sockets[socket].connected || _sockets[socket].open))
    {
        DEBUG_D("socketClose(%d)\r\n", socket);
        sendFormated("AT+CIPCLOSE=%d\r\n", _sockets[socket].handle);
        if (RESP_ERROR == waitFinalResp()) {
        }
        // Assume RESP_OK in most situations, and assume closed
        // already if we couldn't close it, even though this can
        // be false. Recovery added to socketSocket();
        _sockets[socket].connected = false;
        _sockets[socket].connected = false;
        _sockets[socket].open = false;
        ok = true;
    }
    UNLOCK();
    return ok;
}

bool MDMParser::_socketFree(int socket)
{
    bool ok = false;
    LOCK();
    if ((socket >= 0) && (socket < NUMSOCKETS)) {
        if (_sockets[socket].handle != MDM_SOCKET_ERROR) {
            DEBUG_D("socketFree(%d)\r\n",  socket);
            _sockets[socket].handle     = MDM_SOCKET_ERROR;
            _sockets[socket].localip    = 0;
            _sockets[socket].connected  = false;
            _sockets[socket].pending    = 0;
            _sockets[socket].open       = false;
            if (_sockets[socket].pipe)
                delete _sockets[socket].pipe;
        }
        ok = true;
    }
    UNLOCK();
    return ok; // only false if invalid socket
}

bool MDMParser::socketFree(int socket)
{
    // make sure it is closed
    socketClose(socket);
    return _socketFree(socket);
}

int MDMParser::socketSend(int socket, const char * buf, int len)
{
    //DEBUG_D("socketSend(%d,,%d)\r\n", socket,len);
    int cnt = len;
    while (cnt > 0) {
        int blk = USO_MAX_WRITE;
        if (cnt < blk)
            blk = cnt;
        bool ok = false;
        {
            LOCK();
            if (ISSOCKET(socket)) {
                sendFormated("AT+CIPSEND=%d,%d\r\n",_sockets[socket].handle,blk);
                if (RESP_OK == waitFinalResp()){
                    if (RESP_PROMPT == waitFinalResp()) {
                        send(buf, blk);
                        if (RESP_OK == waitFinalResp())
                            ok = true;
                    }
                }
            }
            UNLOCK();
        }
        if (!ok)
            return MDM_SOCKET_ERROR;
        buf += blk;
        cnt -= blk;
    }
    return (len - cnt);
}

int MDMParser::socketSendTo(int socket, MDM_IP ip, int port, const char * buf, int len)
{
    DEBUG_D("socketSendTo(%d," IPSTR ",%d,,%d)\r\n", socket,IPNUM(ip),port,len);
    int cnt = len;
    while (cnt > 0) {
        int blk = USO_MAX_WRITE;
        if (cnt < blk)
            blk = cnt;
        bool ok = false;
        {
            LOCK();
            if (ISSOCKET(socket)) {
                sendFormated("AT+CIPSEND=%d,%d,\"" IPSTR "\",%d,%d\r\n",_sockets[socket].handle, blk, IPNUM(ip),port);
                if (RESP_PROMPT == waitFinalResp()) {
                    HAL_Delay_Milliseconds(50);
                    send(buf, blk);
                    if (RESP_OK == waitFinalResp())
                        ok = true;
                }
            }
            UNLOCK();
        }
        if (!ok)
            return MDM_SOCKET_ERROR;
        buf += blk;
        cnt -= blk;
    }
    return (len - cnt);
}

int MDMParser::socketReadable(int socket)
{
    waitFinalResp(NULL, NULL, 0);
    int pending = MDM_SOCKET_ERROR;
    if (_cancel_all_operations)
        return MDM_SOCKET_ERROR;
    if (ISSOCKET(socket) && _sockets[socket].connected) {
        //DEBUG_D("socketReadable(%d)\r\n", socket);
        // allow to receive unsolicited commands
        if (_sockets[socket].connected)
            pending = _sockets[socket].pending;
    }
    return pending;
}

int MDMParser::socketRecv(int socket, char* buf, int len)
{
    if (ISSOCKET(socket)) {
        if (_sockets[socket].connected) {
            int available = socketReadable(socket);
            if (available>0)  {
                if (len > available)    // only read up to the amount available. When 0,
                    len = available;// skip reading and check timeout.
                _sockets[socket].pipe->get(buf,len,false);
                _sockets[socket].pending -= len;
                return len;
            }
        }
    }
    return 0;
}

int MDMParser::socketRecvFrom(int socket, MDM_IP* ip, int* port, char* buf, int len)
{
    return socketRecv(socket, buf, len);
}

int MDMParser::_findSocket(int handle) {
    for (int socket = 0; socket < NUMSOCKETS; socket ++) {
        if (_sockets[socket].handle == handle)
            return socket;
    }
    return MDM_SOCKET_ERROR;
}

int MDMParser::_cbDownOtaFile(int type, const char* buf, int len, deal_status_t* result)
{
    int rst;
    if (type == TYPE_PLUS) {
        if (sscanf(buf, "+IR_DOWNFILE:%d\r\n", &rst) == 1) {
            *result = (deal_status_t)rst;
        }
    }
    return WAIT;
}

deal_status_t MDMParser::downOtaFile(const char *host, const char *param, const char * md5)
{
    deal_status_t result = DEALSTATUS_FAIL;

    LOCK();
    _downotafile_status = DEALSTATUS_IDLE;
    if (_init) {
        sendFormated("AT+IR_DOWNFILE=\"%s\",\"%s\",\"%s\"\r\n", host, param, md5);
        if (RESP_OK != waitFinalResp(_cbDownOtaFile, &result)){
            result = DEALSTATUS_FAIL;
        }
    }
    _downotafile_status = result;
    UNLOCK();
    return result;
}

/**
 * Used to set ip mux connect
 */
deal_status_t MDMParser::getDownOtafileStatus(void)
{
    waitFinalResp(NULL, NULL, 0);
    return _downotafile_status;
}

int MDMParser::_cbDownNetFile(int type, const char* buf, int len, deal_status_t* result)
{
    int rst;
    if (type == TYPE_PLUS) {
        if (sscanf(buf, "+IR_NETDOWN:%d\r\n", &rst) == 1) {
            *result = (deal_status_t)rst;
        }
    }
    return WAIT;
}

/**
 * Used to set ip mux connect
 */
deal_status_t MDMParser::downNetFile(const char *host, const char *param)
{
    deal_status_t result = DEALSTATUS_FAIL;
    LOCK();
    _downnetfile_status = DEALSTATUS_IDLE;
    if (_init) {
        sendFormated("AT+IR_NETDOWN=\"%s\",\"%s\"\r\n", host, param);
        if (RESP_OK != waitFinalResp(_cbDownNetFile, &result)){
            result = DEALSTATUS_FAIL;
        }
    }
    _downnetfile_status = result;
    UNLOCK();
    return result;
}

/**
 * Used to set ip mux connect
 */
bool MDMParser::updateNet(void)
{
    bool ok = false;
    LOCK();

    if (_init) {
        sendFormated("AT+IR_NETUPDATE\r\n");
        if (RESP_OK == waitFinalResp()){
            ok = true;
        }
    }
    UNLOCK();
    return ok;
}

/**
 * Used to set ip mux connect
 */
deal_status_t MDMParser::getDownNetfileStatus(void)
{
    waitFinalResp(NULL, NULL, 0);
    return _downnetfile_status;
}

int MDMParser::_cbGetDownFileProgress(int type, const char* buf, int len, int* result)
{
    if (type == TYPE_PLUS) {
        if (sscanf(buf, "+IR_DOWNPROGRESS:%d\r\n", result) == 1)
        /*nothing*/;
    }
    return WAIT;
}

/**
 * Used to set ip mux connect
 */
int MDMParser::getDownFileProgress(void)
{
    int result = 0;
    LOCK();

    if (_init) {
        sendFormated("AT+IR_DOWNPROGRESS?\r\n");
        waitFinalResp(_cbGetDownFileProgress, &result);
    }
    UNLOCK();
    return result;
}

int MDMParser::_cbGetBootloaderPacketSize(int type, const char* buf, int len, uint32_t* result)
{
    if (type == TYPE_PLUS) {
        if (sscanf(buf, "+IR_GETFILESIZE:%d\r\n", result) == 1)
        /*nothing*/;
    }
    return WAIT;
}

int MDMParser::_cbGetBootloaderPacket(int type, const char* buf, int len, uint8_t* pdata)
{
    uint32_t size = 0;

    if (type == TYPE_PLUS) {
        if (sscanf(buf, "+IR_GETFILEPACKET,%d\r\n", &size) == 1) {
            char *s = strchr(buf, ':');
            for(int n=0; n < size; n++) {
                memcpy(pdata, s+1, size);
            }
            return RESP_OK;
        }
    }
    return RESP_ERROR;
}

/**
 * Used to get bootloader from esp8266
 */
#define PACKAGE_UNIT 1024  //1k
bool MDMParser::getBootloader(void)
{
    bool ok = false;
    uint8_t buf[1200];
    uint16_t crc16 = 0;
    uint32_t packet_size = 0;
    int PacketIndex = 0;
    LOCK();

    if (_init) {
        sendFormated("AT+IR_GETFILESIZE=%d,2\r\n", PACKAGE_UNIT);
        if (RESP_OK == waitFinalResp(_cbGetBootloaderPacketSize, &packet_size)) {
            DEBUG_D("packet_size = %d\r\n", packet_size);
            if(packet_size) {
                for(PacketIndex = 0; PacketIndex < packet_size; PacketIndex++) {
                    sendFormated("AT+IR_GETFILEPACKET= %d,%d,2\r\n", PACKAGE_UNIT, PacketIndex);
                    memset(buf, 0, sizeof(buf));
                    if (RESP_OK == waitFinalResp(_cbGetBootloaderPacket, buf)) {
                        crc_reset();
                        crc_update_n(buf, PACKAGE_UNIT);
                        crc16 = crc_get_reseult();
                        if( crc16 == ((buf[PACKAGE_UNIT] << 8) | (buf[PACKAGE_UNIT+1]))) {
                            DEBUG_D("crc32 success  flash begin\r\n");
                            if( PacketIndex*PACKAGE_UNIT+CACHE_BOOTLOADER_START_ADDR+PACKAGE_UNIT < APP_ADDR ) {
                                InternalFlashStore flashStore;
                                if( flashStore.write(PacketIndex*PACKAGE_UNIT+CACHE_BOOTLOADER_START_ADDR, (uint32_t *)&buf, PACKAGE_UNIT) ) {
                                    DEBUG_D("crc32 success flash end\r\n");
                                    continue;
                                }
                            }
                        }
                    }
                    break;
                }
                if(PacketIndex == packet_size)
                {
                    ok = false;
                }
            }
        }
    }
    UNLOCK();
    return ok;
}

// ----------------------------------------------------------------
bool MDMParser::setDebug(int level)
{
#ifdef MDM_DEBUG
    if ((_debugLevel >= -1) && (level >= -1) &&
            (_debugLevel <=  3) && (level <=  3)) {
        _debugLevel = level;
        return true;
    }
#endif
    return false;
}

void MDMParser::dumpIp(MDM_IP ip)
{
    if (ip != NOIP) {
        DEBUG_D("\r\n[ Modem:IP " IPSTR " ] = = = = = = = = = = = = = =\r\n", IPNUM(ip));
    }
}

// ----------------------------------------------------------------
int MDMParser::_parseMatch(Pipe<char>* pipe, int len, const char* sta, const char* end)
{
    int o = 0;
    if (sta) {
        while (*sta) {
            if (++o > len)                  return WAIT;
            char ch = pipe->next();
            if (*sta++ != ch)               return NOT_FOUND;
        }
    }
    if (!end)                               return o; // no termination
    // at least any char
    if (++o > len)                      return WAIT;
    pipe->next();
    // check the end
    int x = 0;
    while (end[x]) {
        if (++o > len)                      return WAIT;
        char ch = pipe->next();
        x = (end[x] == ch) ? x + 1 :
            (end[0] == ch) ? 1 :
            0;
    }
    return o;
}

int MDMParser::_parseFormated(Pipe<char>* pipe, int len, const char* fmt)
{
    int o = 0;
    int num = 0;
    if (fmt) {
        while (*fmt) {
            if (++o > len)                  return WAIT;
            char ch = pipe->next();
            if (*fmt == '%') {
                fmt++;
                if (*fmt == 'd') { // numeric
                    fmt ++;
                    num = 0;
                    while (ch >= '0' && ch <= '9') {
                        num = num * 10 + (ch - '0');
                        if (++o > len)      return WAIT;
                        ch = pipe->next();
                    }
                }
                else if (*fmt == 'c') { // char buffer (takes last numeric as length)
                    fmt ++;
                    while (--num) {
                        if (++o > len)      return WAIT;
                        ch = pipe->next();
                    }
                    continue;
                }
                else if (*fmt == 's') {
                    fmt ++;
                    if (ch != '\"')         return NOT_FOUND;
                    do {
                        if (++o > len)      return WAIT;
                        ch = pipe->next();
                    } while (ch != '\"');
                    if (++o > len)          return WAIT;
                    ch = pipe->next();
                }
            }
            if (*fmt++ != ch)               return NOT_FOUND;
        }
    }
    return o;
}

int MDMParser::_getLine(Pipe<char>* pipe, char* buf, int len)
{
    int unkn = 0;
    int sz = pipe->size();
    int fr = pipe->free();
    if (len > sz)
        len = sz;
    while (len > 0)
    {
        static struct {
            const char* fmt;                              int type;
        } lutF[] = {
            { "+IPD,%d,%d:%c",                          TYPE_PLUS       },
            { "+IR_GETFILEPACKET,%d:%c",                TYPE_PLUS       },
        };
        static struct {
            const char* sta;          const char* end;    int type;
        } lut[] = {
            { "\r\nOK\r\n",                          NULL,               TYPE_OK            },
            { "\r\nERROR\r\n",                       NULL,               TYPE_ERROR         },
            { "\r\nFAIL\r\n",                        NULL,               TYPE_FAIL          },
            { "\r\nALREAY CONNECT\r\n",              NULL,               TYPE_CONNECT       },
            { "WIFI CONNECTED\r\n",                  NULL,               TYPE_CONNECT       },
            { "WIFI GOT IP\r\n",                     NULL,               TYPE_DHCP          },
            { "WIFI DISCONNECT\r\n",                 NULL,               TYPE_DISCONNECT    },
            { "\r\nbusy p...\r\n",                   NULL,               TYPE_BUSY          },
            { "smartconfig connected wifi\r\n",      NULL,               TYPE_SMARTCONFIG   },
            { "+",                                   "\r\n",             TYPE_PLUS          },
            { "> ",                                  NULL,               TYPE_PROMPT        }, // Sockets
            { "\r\nSEND OK\r\n",                     NULL,               TYPE_OK            }, // Sockets
        };
        for (int i = 0; i < (int)(sizeof(lutF)/sizeof(*lutF)); i ++) {
            pipe->set(unkn);
            int ln = _parseFormated(pipe, len, lutF[i].fmt);
            if (ln == WAIT && fr)
                return WAIT;
            if ((ln != NOT_FOUND) && (unkn > 0))
                return TYPE_UNKNOWN | pipe->get(buf, unkn);
            if (ln > 0)
                return lutF[i].type  | pipe->get(buf, ln);
        }
        for (int i = 0; i < (int)(sizeof(lut)/sizeof(*lut)); i ++) {
            pipe->set(unkn);
            int ln = _parseMatch(pipe, len, lut[i].sta, lut[i].end);
            if (ln == WAIT && fr)
                return WAIT;
            if ((ln != NOT_FOUND) && (unkn > 0))
                return TYPE_UNKNOWN | pipe->get(buf, unkn);
            if (ln > 0)
                return lut[i].type | pipe->get(buf, ln);
        }
        // UNKNOWN
        unkn ++;
        len--;
    }
    return TYPE_UNKNOWN | pipe->get(buf, unkn); //应该返回TYPE_UNKNOWN 并且应该从缓存里面清掉。否则会一直接受到相同的数据 并且应该从缓存里面清掉。否则会一直接受到相同的数据 并且应该从缓存里面清掉。否则会一直接受到相同的数据 并且应该从缓存里面清掉。否则会一直处理相同的数据  chenkaiyao  2016-01-09
    //return WAIT;
}

// ----------------------------------------------------------------
// Electron Serial Implementation
// ----------------------------------------------------------------

MDMEsp8266Serial::MDMEsp8266Serial(int rxSize /*= 256*/, int txSize /*= 256*/) :
    Esp8266SerialPipe(rxSize, txSize)
{
#ifdef MDM_DEBUG
    //_debugLevel = -1;
#endif

    // Important to set _dev.lpm = LPM_ENABLED; when HW FLOW CONTROL enabled.
}

MDMEsp8266Serial::~MDMEsp8266Serial(void)
{
}

int MDMEsp8266Serial::_send(const void* buf, int len)
{
    return put((const char*)buf, len, true/*=blocking*/);
}

int MDMEsp8266Serial::getLine(char* buffer, int length)
{
    return _getLine(&_pipeRx, buffer, length);
}

