/* ESP8266 Example
 * Copyright (c) 2015 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ESP8266.h"
#include <cstring>

#define   ESP8266_DEFAULT_BAUD_RATE   115200

ESP8266::ESP8266(PinName tx, PinName rx, bool debug)
    : _serial(tx, rx, ESP8266_DEFAULT_BAUD_RATE), 
      _parser(&_serial), 
      _packets(0), 
      _packets_end(&_packets),
      _connect_error(0),
      _fail(false)
{
    _serial.set_baud( ESP8266_DEFAULT_BAUD_RATE );
    _parser.debug_on(debug);
    _parser.set_delimiter("\r\n");
    _parser.oob("+IPD", callback(this, &ESP8266::_packet_handler));
    //Note: espressif at command document says that this should be +CWJAP_CUR:<error code>
    //but seems that at least current version is not sending it
    //https://www.espressif.com/sites/default/files/documentation/4a-esp8266_at_instruction_set_en.pdf
    //Also seems that ERROR is not sent, but FAIL instead
    _parser.oob("+CWJAP:", callback(this, &ESP8266::_connect_error_handler));
}

int ESP8266::get_firmware_version()
{
    _parser.send("AT+GMR");
    int version;
    if (_parser.recv("SDK version:%d", &version) && _parser.recv("OK")) {
        return version;
    } else { 
        // Older firmware versions do not prefix the version with "SDK version: "
        return -1;
    }
    
}

bool ESP8266::startup(int mode)
{
    //only 3 valid modes
    if (mode < 1 || mode > 3) {
        return false;
    }

    return _parser.send("AT+CWMODE_CUR=%d", mode)
        && _parser.recv("OK")
        && _parser.send("AT+CIPMUX=1")
        && _parser.recv("OK");
}

bool ESP8266::reset(void)
{
    for (int i = 0; i < 2; i++) {
        if (_parser.send("AT+RST")
            && _parser.recv("OK\r\nready")) {
            return true;
        }
    }

    return false;
}

bool ESP8266::dhcp(bool enabled, int mode)
{
    //only 3 valid modes
    if (mode < 0 || mode > 2) {
        return false;
    }

    return _parser.send("AT+CWDHCP_CUR=%d,%d", mode, enabled?1:0)
        && _parser.recv("OK");
}

nsapi_error_t ESP8266::connect(const char *ap, const char *passPhrase)
{
    _parser.send("AT+CWJAP_CUR=\"%s\",\"%s\"", ap, passPhrase);
    if (!_parser.recv("OK")) {
        if (_fail) {
            nsapi_error_t ret;
            if (_connect_error == 1)
                ret = NSAPI_ERROR_CONNECTION_TIMEOUT;
            else if (_connect_error == 2)
                ret = NSAPI_ERROR_AUTH_FAILURE;
            else if (_connect_error == 3)
                ret = NSAPI_ERROR_NO_SSID;
            else
                ret = NSAPI_ERROR_NO_CONNECTION;

            _fail = false;
            _connect_error = 0;
            return ret;
        }
    }

    return NSAPI_ERROR_OK;
}

bool ESP8266::disconnect(void)
{
    return _parser.send("AT+CWQAP") && _parser.recv("OK");
}

const char *ESP8266::getIPAddress(void)
{
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAIP,\"%15[^\"]\"", _ip_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _ip_buffer;
}

const char *ESP8266::getMACAddress(void)
{
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAMAC,\"%17[^\"]\"", _mac_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _mac_buffer;
}

const char *ESP8266::getGateway()
{
    if (!(_parser.send("AT+CIPSTA_CUR?")
        && _parser.recv("+CIPSTA_CUR:gateway:\"%15[^\"]\"", _gateway_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _gateway_buffer;
}

const char *ESP8266::getNetmask()
{
    if (!(_parser.send("AT+CIPSTA_CUR?")
        && _parser.recv("+CIPSTA_CUR:netmask:\"%15[^\"]\"", _netmask_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _netmask_buffer;
}

int8_t ESP8266::getRSSI()
{
    int8_t rssi;
    char bssid[18];

   if (!(_parser.send("AT+CWJAP_CUR?")
        && _parser.recv("+CWJAP_CUR:\"%*[^\"]\",\"%17[^\"]\"", bssid)
        && _parser.recv("OK"))) {
        return 0;
    }

    if (!(_parser.send("AT+CWLAP=\"\",\"%s\",", bssid)
        && _parser.recv("+CWLAP:(%*d,\"%*[^\"]\",%hhd,", &rssi)
        && _parser.recv("OK"))) {
        return 0;
    }

    return rssi;
}

bool ESP8266::isConnected(void)
{
    const char *ip_buff = getIPAddress();

    if(!ip_buff || std::strcmp(ip_buff, "0.0.0.0") == 0) {
        return false;
    }

    return true;
}

int ESP8266::scan(WiFiAccessPoint *res, unsigned limit)
{
    unsigned cnt = 0;
    nsapi_wifi_ap_t ap;

    if (!_parser.send("AT+CWLAP")) {
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    while (recv_ap(&ap)) {
        if (cnt < limit) {
            res[cnt] = WiFiAccessPoint(ap);
        }

        cnt++;
        if (limit != 0 && cnt >= limit) {
            break;
        }
    }

    return cnt;
}

bool ESP8266::open(const char *type, int id, const char* addr, int port)
{
    //IDs only 0-4
    if (id > 4) {
        return false;
    }
    return _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d", id, type, addr, port)
        && _parser.recv("OK");
}

bool ESP8266::dns_lookup(const char* name, char* ip)
{
    return _parser.send("AT+CIPDOMAIN=\"%s\"", name) && _parser.recv("+CIPDOMAIN:%s%*[\r]%*[\n]", ip);
}

bool ESP8266::send(int id, const void *data, uint32_t amount)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        if (_parser.send("AT+CIPSEND=%d,%d", id, amount)
            && _parser.recv(">")
            && _parser.write((char*)data, (int)amount) >= 0) {
            return true;
        }
    }

    return false;
}

void ESP8266::_packet_handler()
{
    int id;
    uint32_t amount;

    // parse out the packet
    if (!_parser.recv(",%d,%d:", &id, &amount)) {
        return;
    }

    struct packet *packet = (struct packet*)malloc(
            sizeof(struct packet) + amount);
    if (!packet) {
        return;
    }

    packet->id = id;
    packet->len = amount;
    packet->next = 0;

    if (!(_parser.read((char*)(packet + 1), amount))) {
        free(packet);
        return;
    }

    // append to packet list
    *_packets_end = packet;
    _packets_end = &packet->next;
}

int32_t ESP8266::recv(int id, void *data, uint32_t amount)
{
    while (true) {
        // check if any packets are ready for us
        for (struct packet **p = &_packets; *p; p = &(*p)->next) {
            if ((*p)->id == id) {
                struct packet *q = *p;

                if (q->len <= amount) { // Return and remove full packet
                    memcpy(data, q+1, q->len);

                    if (_packets_end == &(*p)->next) {
                        _packets_end = p;
                    }
                    *p = (*p)->next;

                    uint32_t len = q->len;
                    free(q);
                    return len;
                } else { // return only partial packet
                    memcpy(data, q+1, amount);

                    q->len -= amount;
                    memmove(q+1, (uint8_t*)(q+1) + amount, q->len);

                    return amount;
                }
            }
        }

        // Check for inbound packets
        if (!_parser.process_oob()) {
            return -1;
        }
    }
}

bool ESP8266::close(int id)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        if (_parser.send("AT+CIPCLOSE=%d", id)
            && _parser.recv("OK")) {
            return true;
        }
    }

    return false;
}

void ESP8266::setTimeout(uint32_t timeout_ms)
{
    _parser.set_timeout(timeout_ms);
}

bool ESP8266::readable()
{
    return _serial.FileHandle::readable();
}

bool ESP8266::writeable()
{
    return _serial.FileHandle::writable();
}

void ESP8266::attach(Callback<void()> func)
{
    _serial.sigio(func);
}

bool ESP8266::recv_ap(nsapi_wifi_ap_t *ap)
{
    int sec;
    bool ret = _parser.recv("+CWLAP:(%d,\"%32[^\"]\",%hhd,\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%d", &sec, ap->ssid,
                            &ap->rssi, &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4],
                            &ap->bssid[5], &ap->channel);

    ap->security = sec < 5 ? (nsapi_security_t)sec : NSAPI_SECURITY_UNKNOWN;

    return ret;
}

void ESP8266::_connect_error_handler()
{
    _fail = false;
    _connect_error = 0;

    if (_parser.recv("%d", &_connect_error) && _parser.recv("FAIL")) {
        _fail = true;
        _parser.abort();
    }
}
