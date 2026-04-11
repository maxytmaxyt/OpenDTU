// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "NetworkSettings.h"
#include "Configuration.h"
#include "SyslogLogger.h"
#include "PinMapping.h"
#include "Utils.h"
#include "__compiled_constants.h"
#include "defaults.h"
#include <ESPmDNS.h>
#include <ETH.h>

#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/lwip_napt.h"
#include "esp_wifi.h"  // Wichtig für SOFTAP_IF

// Wir definieren die fehlende Konstante zur Sicherheit selbst, 
// falls der Header sie im Arduino-Kontext versteckt
#ifndef SOFTAP_IF
#define SOFTAP_IF ESP_IF_WIFI_AP
#endif

#ifndef DOMAIN_NAME_SERVER
#define DOMAIN_NAME_SERVER 6
#endif

// Diese Funktion muss als extern "C" deklariert werden, 
// damit der C++ Compiler sie in den C-Bibliotheken findet
extern "C" {
    #include "dhcpserver/dhcpserver.h"
    err_t ip_napt_init(uint16_t max_nat, uint16_t max_chained);
    err_t ip_napt_enable_no(uint8_t iface, uint8_t enable);
}
#undef TAG
static const char* TAG = "network";

NetworkSettingsClass::NetworkSettingsClass()
    : _loopTask(TASK_IMMEDIATE, TASK_FOREVER, std::bind(&NetworkSettingsClass::loop, this))
    , _apIp(192, 168, 4, 1)
    , _apNetmask(255, 255, 255, 0)
    , _dnsServer(std::make_unique<DNSServer>())
{
}

void NetworkSettingsClass::init(Scheduler& scheduler)
{
    using std::placeholders::_1;
    using std::placeholders::_2;

    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    WiFi.disconnect(true, true);

    WiFi.onEvent(std::bind(&NetworkSettingsClass::NetworkEvent, this, _1, _2));

    if (PinMapping.isValidW5500Config()) {
        const PinMapping_t& pin = PinMapping.get();
        _w5500 = W5500::setup(pin.w5500_mosi, pin.w5500_miso, pin.w5500_sclk, pin.w5500_cs, pin.w5500_int, pin.w5500_rst);
        if (_w5500)
            ESP_LOGI(TAG, "W5500: Connection successful");
        else
            ESP_LOGE(TAG, "W5500: Connection error!!");
    }
#if CONFIG_ETH_USE_ESP32_EMAC
    else if (PinMapping.isValidEthConfig()) {
        const PinMapping_t& pin = PinMapping.get();
#if ESP_ARDUINO_VERSION_MAJOR < 3
        ETH.begin(pin.eth_phy_addr, pin.eth_power, pin.eth_mdc, pin.eth_mdio, pin.eth_type, pin.eth_clk_mode);
#else
        ETH.begin(pin.eth_type, pin.eth_phy_addr, pin.eth_mdc, pin.eth_mdio, pin.eth_power, pin.eth_clk_mode);
#endif
    }
#endif

    setupMode();

    scheduler.addTask(_loopTask);
    _loopTask.enable();

    Syslog.init(scheduler);
}

void NetworkSettingsClass::NetworkEvent(const WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
        ESP_LOGI(TAG, "ETH start");
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_START);
        }
        break;
    case ARDUINO_EVENT_ETH_STOP:
        ESP_LOGI(TAG, "ETH stop");
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_STOP);
        }
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        ESP_LOGI(TAG, "ETH connected");
        _ethConnected = true;
        raiseEvent(network_event::NETWORK_CONNECTED);
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
        ESP_LOGI(TAG, "ETH got IP: %s", ETH.localIP().toString().c_str());
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_GOT_IP);
        }
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        ESP_LOGI(TAG, "ETH disconnected");
        _ethConnected = false;
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_DISCONNECTED);
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        if (_networkMode == network_mode::WiFi) {
            raiseEvent(network_event::NETWORK_CONNECTED);
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi disconnected: %" PRIu8 "", info.wifi_sta_disconnected.reason);
        if (_networkMode == network_mode::WiFi) {
            ESP_LOGI(TAG, "Try reconnecting");
            _lastReconnectAttempt = millis();
            WiFi.disconnect(true, false);
            WiFi.begin();
            raiseEvent(network_event::NETWORK_DISCONNECTED);
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        ESP_LOGI(TAG, "WiFi got ip: %s", WiFi.localIP().toString().c_str());
        if (_networkMode == network_mode::WiFi) {
            raiseEvent(network_event::NETWORK_GOT_IP);
        }
        break;
    default:
        break;
    }
}

bool NetworkSettingsClass::onEvent(DtuNetworkEventCb cbEvent, const network_event event)
{
    if (!cbEvent) {
        return pdFALSE;
    }
    DtuNetworkEventCbList_t newEventHandler;
    newEventHandler.cb = cbEvent;
    newEventHandler.event = event;
    _cbEventList.push_back(newEventHandler);
    return true;
}

void NetworkSettingsClass::raiseEvent(const network_event event)
{
    for (auto& entry : _cbEventList) {
        if (entry.cb) {
            if (entry.event == event || entry.event == network_event::NETWORK_EVENT_MAX) {
                entry.cb(event);
            }
        }
    }
}

void NetworkSettingsClass::handleMDNS()
{
    const bool mdnsEnabled = Configuration.get().Mdns.Enabled;

    if (_lastMdnsEnabled == mdnsEnabled) {
        return;
    }

    _lastMdnsEnabled = mdnsEnabled;
    MDNS.end();

    if (!mdnsEnabled) {
        ESP_LOGI(TAG, "MDNS disabled");
        return;
    }

    ESP_LOGI(TAG, "Starting MDNS responder...");

    if (!MDNS.begin(getHostname())) {
        ESP_LOGE(TAG, "Error setting up MDNS responder!");
        return;
    }

    MDNS.addService("http", "tcp", 80);
    MDNS.addService("opendtu", "tcp", 80);
    MDNS.addServiceTxt("opendtu", "tcp", "git_hash", __COMPILED_GIT_HASH__);

    ESP_LOGI(TAG, "MDNS started");
}

void NetworkSettingsClass::setupMode()
{
    if (_adminEnabled) {
        WiFi.mode(WIFI_AP_STA);
        String ssidString = getApName();
        WiFi.softAPConfig(_apIp, _apIp, _apNetmask);
        WiFi.softAP(ssidString.c_str(), Configuration.get().Security.Password);

        // --- NAPT / REPEATER LOGIC ---
        // Initialize NAPT to share internet from Station to Access Point
        ip_napt_init(32, 16);
        ip_napt_enable_no(SOFTAP_IF, 1);

        // Configure DNS for connected clients (Google DNS as fallback)
        dhcps_lease_t lease;
        lease.enable = true;
        IPAddress dns(8, 8, 8, 8);
        dhcps_set_option_info(DOMAIN_NAME_SERVER, &dns, sizeof(dns));
        // -----------------------------

        _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
        _dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());
        _dnsServerStatus = true;
    } else {
        _dnsServerStatus = false;
        _dnsServer->stop();
        if (_networkMode == network_mode::WiFi) {
            WiFi.mode(WIFI_STA);
        } else {
            WiFi.mode(WIFI_MODE_NULL);
        }
    }
}

void NetworkSettingsClass::enableAdminMode()
{
    _connectTimeoutTimer = 0;
    _connectRedoTimer = 0;

    _adminTimeoutCounter = 0;
    // Set timeout to 0 (disabled) to ensure the AP stays alive as a repeater
    _adminTimeoutCounterMax = 0; 
    _adminEnabled = true;
    setupMode();
}

void NetworkSettingsClass::disableAdminMode()
{
    // If we want to use it as a repeater, we ignore the disable request
    // unless explicitly needed. For your case, we keep it enabled.
    if (_networkMode == network_mode::WiFi) {
        ESP_LOGI(TAG, "Repeater mode active: ignoring disableAdminMode");
        return; 
    }
    _adminEnabled = false;
    ESP_LOGI(TAG, "Admin mode disabled");
    setupMode();
}

bool NetworkSettingsClass::wifiConfigured() const
{
    return strcmp(Configuration.get().WiFi.Ssid, "");
}

String NetworkSettingsClass::getApName() const
{
    return String(ACCESS_POINT_NAME + String(Utils::getChipId()));
}

void NetworkSettingsClass::loop()
{
    if (_ethConnected) {
        if (_networkMode != network_mode::Ethernet) {
            ESP_LOGI(TAG, "Switch to Ethernet mode");
            _networkMode = network_mode::Ethernet;
            WiFi.mode(WIFI_MODE_NULL);
            setStaticIp();
            setHostname();
        }
    } else if (_networkMode != network_mode::WiFi) {
        ESP_LOGI(TAG, "Switch to WiFi mode");
        _networkMode = network_mode::WiFi;
        enableAdminMode();
        applyConfig();
    }

    if (millis() - _lastTimerCall > 1000) {
        // Admin timeout logic modified: only count if _adminTimeoutCounterMax > 0
        if (_adminEnabled && _adminTimeoutCounterMax > 0) {
            _adminTimeoutCounter++;
            if (_adminTimeoutCounter % 10 == 0) {
                ESP_LOGI(TAG, "Admin AP remaining seconds: %" PRIu32 " / %" PRIu32 "", _adminTimeoutCounter, _adminTimeoutCounterMax);
            }
        }
        if (_performConnection && !isConnected() && wifiConfigured() && millis() - _lastReconnectAttempt > 60000) {
            ESP_LOGW(TAG, "Wifi reconnect watchdog triggered... Resetting Wifi hardware");
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_MODE_NULL);
            if (_adminEnabled) {
                enableAdminMode();
            }
            applyConfig();
            _lastReconnectAttempt = millis();
        }
        _connectTimeoutTimer++;
        _connectRedoTimer++;
        _lastTimerCall = millis();
    }

    if (_adminEnabled) {
        if (!isConnected()) {
            _adminTimeoutCounter = 0;
        }
        // AP will only disable if _adminTimeoutCounterMax is explicitly set > 0
        if (_adminTimeoutCounterMax > 0 && _adminTimeoutCounter > _adminTimeoutCounterMax) {
            disableAdminMode();
        }

        if (isConnected()) {
            _connectTimeoutTimer = 0;
            _connectRedoTimer = 0;
        } else {
            if (_connectTimeoutTimer > WIFI_RECONNECT_TIMEOUT && _performConnection) {
                ESP_LOGI(TAG, "Disabling search for AP...");
                WiFi.mode(WIFI_AP);
                _connectRedoTimer = 0;
                _performConnection = false;
            }
            if (_connectRedoTimer > WIFI_RECONNECT_REDO_TIMEOUT && !_performConnection) {
                ESP_LOGI(TAG, "Enable search for AP...");
                WiFi.mode(WIFI_AP_STA);
                applyConfig();
                _connectTimeoutTimer = 0;
                _performConnection = true;
            }
        }
    }
    if (_dnsServerStatus) {
        _dnsServer->processNextRequest();
    }

    handleMDNS();
}

void NetworkSettingsClass::applyConfig()
{
    setHostname();
    const auto& config = Configuration.get().WiFi;
    if (!wifiConfigured()) {
        return;
    }

    const bool newCredentials = strcmp(WiFi.SSID().c_str(), config.Ssid) || strcmp(WiFi.psk().c_str(), config.Password);

    ESP_LOGI(TAG, "Start configuring WiFi STA using %s credentials",
        newCredentials ? "new" : "existing");

    bool success = false;
    if (newCredentials) {
        success = WiFi.begin(
            config.Ssid,
            config.Password) != WL_CONNECT_FAILED;
    } else {
        success = WiFi.begin() != WL_CONNECT_FAILED;
    }

    ESP_LOG_LEVEL_LOCAL((success ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "Configuring WiFi %s", success ? "done" : "failed");
    setStaticIp();
    Syslog.updateSettings(getHostname());
}

void NetworkSettingsClass::setHostname()
{
    if (_networkMode == network_mode::Undefined) {
        return;
    }
    const String hostname = getHostname();
    bool success = false;
    ESP_LOGI(TAG, "Start setting hostname...");
    if (_networkMode == network_mode::WiFi) {
        success = WiFi.hostname(hostname);
        WiFi.mode(WIFI_MODE_APSTA);
        WiFi.mode(WIFI_MODE_STA);
        setupMode();
    } else if (_networkMode == network_mode::Ethernet) {
        success = ETH.setHostname(hostname.c_str());
    }
    ESP_LOG_LEVEL_LOCAL((success ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "Setting hostname %s", success ? "done" : "failed");
}

void NetworkSettingsClass::setStaticIp()
{
    if (_networkMode == network_mode::Undefined) {
        return;
    }
    const auto& config = Configuration.get().WiFi;
    const char* mode = (_networkMode == network_mode::WiFi) ? "WiFi" : "Ethernet";
    const char* ipType = config.Dhcp ? "DHCP" : "static";
    ESP_LOGI(TAG, "Start configuring %s %s IP...", mode, ipType);
    bool success = false;
    if (_networkMode == network_mode::WiFi) {
        if (config.Dhcp) {
            success = WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
        } else {
            success = WiFi.config(
                IPAddress(config.Ip),
                IPAddress(config.Gateway),
                IPAddress(config.Netmask),
                IPAddress(config.Dns1),
                IPAddress(config.Dns2));
        }
    } else if (_networkMode == network_mode::Ethernet) {
        if (config.Dhcp) {
            success = ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
        } else {
            success = ETH.config(
                IPAddress(config.Ip),
                IPAddress(config.Gateway),
                IPAddress(config.Netmask),
                IPAddress(config.Dns1),
                IPAddress(config.Dns2));
        }
    }
    ESP_LOG_LEVEL_LOCAL((success ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "Configure IP %s", success ? "done" : "failed");
}

IPAddress NetworkSettingsClass::localIP() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.localIP();
    case network_mode::WiFi:
        return WiFi.localIP();
    default:
        return INADDR_NONE;
    }
}

IPAddress NetworkSettingsClass::subnetMask() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.subnetMask();
    case network_mode::WiFi:
        return WiFi.subnetMask();
    default:
        return IPAddress(255, 255, 255, 0);
    }
}

IPAddress NetworkSettingsClass::gatewayIP() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.gatewayIP();
    case network_mode::WiFi:
        return WiFi.gatewayIP();
    default:
        return INADDR_NONE;
    }
}

IPAddress NetworkSettingsClass::dnsIP(const uint8_t dns_no) const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.dnsIP(dns_no);
    case network_mode::WiFi:
        return WiFi.dnsIP(dns_no);
    default:
        return INADDR_NONE;
    }
}

String NetworkSettingsClass::macAddress() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        if (_w5500) {
            return _w5500->macAddress();
        }
        return ETH.macAddress();
    case network_mode::WiFi:
        return WiFi.macAddress();
    default:
        return "";
    }
}

String NetworkSettingsClass::getHostname()
{
    const CONFIG_T& config = Configuration.get();
    char preparedHostname[WIFI_MAX_HOSTNAME_STRLEN + 1];
    char resultHostname[WIFI_MAX_HOSTNAME_STRLEN + 1];
    uint8_t pos = 0;
    const uint32_t chipId = Utils::getChipId();
    snprintf(preparedHostname, WIFI_MAX_HOSTNAME_STRLEN + 1, config.WiFi.Hostname, chipId);
    const char* pC = preparedHostname;
    while (*pC && pos < WIFI_MAX_HOSTNAME_STRLEN) {
        if (isalnum(*pC)) {
            resultHostname[pos] = *pC;
            pos++;
        } else if (*pC == ' ' || *pC == '_' || *pC == '-' || *pC == '+' || *pC == '!' || *pC == '?' || *pC == '*') {
            resultHostname[pos] = '-';
            pos++;
        }
        pC++;
    }
    resultHostname[pos] = '\0';
    while (pos > 0 && resultHostname[pos - 1] == '-') {
        resultHostname[pos - 1] = '\0';
        pos--;
    }
    if (strlen(resultHostname) == 0) {
        snprintf(resultHostname, WIFI_MAX_HOSTNAME_STRLEN + 1, APP_HOSTNAME, chipId);
    }
    return resultHostname;
}

bool NetworkSettingsClass::isConnected() const
{
    return (WiFi.localIP()[0] != 0 && WiFi.isConnected() ) || ETH.localIP()[0] != 0;
}

network_mode NetworkSettingsClass::NetworkMode() const
{
    return _networkMode;
}

NetworkSettingsClass NetworkSettings;
