#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <SPI.h>
#include <RF24.h>
#include <ESPmDNS.h>
#include <queue>

// NRF24L01 pins
#define CE_PIN   17
#define CSN_PIN  5
#define SCK_PIN  18
#define MOSI_PIN 23
#define MISO_PIN 19

RF24 radio(CE_PIN, CSN_PIN);
WebSocketsServer webSocket = WebSocketsServer(81);
WebServer server(80);

byte address[6] = "00001";
int channel = 120;
String messageBuffer = "";
std::queue<String> sendBuffer;
bool receivingMessage = false;
String ssid = "ChatRF_1";
String password = "12345678";
bool broadcastSSID = true;
int maxRetries = 5;

void setup() {
    Serial.begin(115200);

    WiFi.softAP(ssid.c_str(), password.c_str());
    Serial.println("Wi-Fi başlatılıyor...");

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("WebSocket başlatıldı!");

    server.on("/", handleRoot);
    server.begin();
    Serial.println("Web sunucusu başlatıldı!");

    if (!MDNS.begin("chatrf")) {  // Domain: chatrf.local
        Serial.println("mDNS başlatılamadı.");
    } else {
        Serial.println("mDNS başlatıldı: http://chatrf.local");
    }
    
    // NRF24L01 ayarları
    radio.begin();
    radio.setDataRate(RF24_2MBPS);
    radio.openWritingPipe(address);
    radio.openReadingPipe(1, address);
    radio.setChannel(channel);
    radio.setPALevel(RF24_PA_HIGH);
    radio.startListening();
    Serial.println("NRF24L01 başlatıldı!");
}

void loop() {
    webSocket.loop();
    server.handleClient();

    if (radio.available()) {
        char receivedMessage[32] = {0};
        radio.read(&receivedMessage, sizeof(receivedMessage));
        Serial.println("Mesaj alındı: " + String(receivedMessage));

        String part = String(receivedMessage);
        receivingMessage = true;

        if (part.startsWith("CHANNEL:")) {
            int newChannel = part.substring(8).toInt();
            Serial.println("Yeni Kanal Bilgisi Alındı: " + String(newChannel));

            radio.setChannel(newChannel);
            channel = newChannel;
            Serial.println("Kanal " + String(channel) + " olarak güncellendi.");

            webSocket.broadcastTXT("CHANNEL:" + String(channel));
        }
        else if (part == "!END!") {
            Serial.println("Son parça alındı: " + messageBuffer);
            webSocket.broadcastTXT(messageBuffer);
            messageBuffer = "";
            receivingMessage = false;
            delay(100);
            sendBufferedMessages();
        } 
        else {
            messageBuffer += part;
            Serial.println("Mesaj birleştiriliyor, devam eden parça: " + messageBuffer);
        }
    }
}

void sendBufferedMessages() {
    while (!sendBuffer.empty()) {
        String message = sendBuffer.front();
        sendBuffer.pop();
        sendMessageInChunks(message);
        delay(100);
    }
}

void sendOrBufferMessage(String message) {
    if (receivingMessage) {
        sendBuffer.push(message);
    } else {
        sendMessageInChunks(message);
    }
}

// WebSocket
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            Serial.println("Yeni bir bağlantı açıldı!");
            break;
        case WStype_DISCONNECTED:
            Serial.println("Bağlantı kapatıldı!");
            break;
        case WStype_TEXT:
            String message = String((char*)payload);
            
            if (message == "GET_CHANNEL") {
                webSocket.broadcastTXT("CHANNEL:" + String(channel));
            } else if (message.startsWith("SETTINGS:")) {
                handleSettings(message.substring(9));
            } else if (message.startsWith("CHANNEL:")) {
                handleChannelSettings(message.substring(8));
            } else {
                if (receivingMessage) {
                    sendOrBufferMessage(message);
                } else {
                    sendMessageInChunks(message);
                }
            }
            break;
    }
}

void handleSettings(String settings) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, settings);

    ssid = doc["ssid"].as<String>();
    password = doc["password"].as<String>();
    broadcastSSID = doc["broadcast"].as<String>() == "true";

    Serial.println("Yeni Ayarlar:");
    Serial.println("SSID: " + ssid);
    Serial.println("Password: " + password);
    Serial.println("SSID Broadcast: " + String(broadcastSSID));
    WiFi.softAP(ssid.c_str(), password.c_str(), !broadcastSSID);
    Serial.println("SSID ve şifre güncellendi.");
}

void handleChannelSettings(String channelSetting) {
    int newChannel = channelSetting.toInt();
    Serial.println("Yeni Kanal Ayarı Alındı: " + String(newChannel));
    String channelMessage = "CHANNEL:" + String(newChannel);
    bool sendSuccess = sendMessageWithRetry(channelMessage);

    if (sendSuccess) {
        Serial.println("Kanal değişikliği sinyali başarıyla gönderildi.");
        radio.setChannel(newChannel);
        channel = newChannel;
        Serial.println("Kanal " + String(channel) + " olarak güncellendi.");
        webSocket.broadcastTXT("CHANNEL:" + String(channel));
    } else {
        Serial.println("Kanal değişikliği sinyali gönderilemedi, kanal değiştirilmeyecek.");
    }
}

bool sendMessageWithRetry(String packet) {
    bool success = false;
    int retryCount = 0;

    while (!success && retryCount < maxRetries) {
        if (radio.write(packet.c_str(), packet.length() + 1)) {
            success = true;
            Serial.println("Mesaj başarıyla iletildi: " + packet);
        } else {
            retryCount++;
            Serial.println("Gönderme başarısız, yeniden deneme: " + String(retryCount));

            delay(100);
        }
    }

    return success;
}

// Mesajı parçalara ayırıp NRF24L01 ile gönder
void sendMessageInChunks(String message) {
    Serial.println("Mesaj parçalanıyor: " + message);
    radio.stopListening();

    int maxLength = 31;
    int messageLength = message.length();

    for (int i = 0; i < messageLength; i += maxLength) {
        String messagePart = message.substring(i, i + maxLength);

        if (sendMessageWithRetry(messagePart)) {
            Serial.println("Mesaj parçası gönderildi: " + messagePart);
        } else {
            Serial.println("Mesaj parçası başarısız oldu.");
        }
    }

    if (sendMessageWithRetry("!END!")) {
        Serial.println("Tüm mesajlar gönderildi: !END!");
    } else {
        Serial.println("!END! mesajı gönderilemedi.");
    }

    radio.startListening();
}

// HTML arayüzü
const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Chat</title>
    <meta charset="UTF-8">
    <style>
        html {
            scroll-behavior: smooth;
        }
        body {
            background-color: #242529;
            font-family: Arial, sans-serif;
            color: #d3d9d4;
            display: flex;
            flex-direction: column;
            height: 100vh;
            margin: 0;
        }
        h1 {
            font-size: 20px;
            margin: 12px;
            text-align: center;
        }
        #chat {
            background-color: #2f3136;
            scroll-behavior: smooth;
            display: flex;
            flex-direction: column;
            width: 100%;
            flex-grow: 1;
            overflow-y: auto;
        }
        .message {
            margin: 8px 0;
            padding: 4px;
            max-width: 60%;
            word-wrap: break-word;
            background-color: #004940;
            color: #d3d9d4;
            border-radius: 10px;
            align-self: flex-end;
            font-size: 16px;
        }
        .message2 {
            margin: 8px 0;
            padding: 4px;
            max-width: 60%;
            word-wrap: break-word;
            background-color: #36393e;
            color: #d3d9d4;
            border-radius: 10px;
            align-self: flex-start;
            font-size: 16px;
        }
        .send-button {
            background-color: #20c20e;
            color: #d3d9d4;
            border: none;
            cursor: pointer;
            height: 40px;
            width: 40px;
            margin-right: 4px;
            border-radius: 50%;
            background-repeat: no-repeat;
            background-size: 60%;
            background-position: center;
            background-position-x: 8px;
            background-image: url('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAMAAACdt4HsAAAAA3NCSVQICAjb4U/gAAAACXBIWXMAAAGnAAABpwGoj3xfAAAAGXRFWHRTb2Z0d2FyZQB3d3cuaW5rc2NhcGUub3Jnm+48GgAAAIRQTFRF////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAvSgy4QAAACt0Uk5TAAEGChgZGhsrLDY3ODk8PU1OW1xdX3CAgYKSo6SlprTFycrL1ufo6ff5+hTxTjgAAAD+SURBVFjD7ZdXEoJAEERXEROYEyoYUFC3738//6wSdtnQpV/0AbrmbZjpEaKVmLAGuK27nAHw2IecASBPMWcA+JPgozIJOQNPEnwrdyZBVa4kqMuNBErlqy5n4EACrWQWcwaWJGiWmQQmySziDEwksFK563EGDSSw11VJAhepSJ5ODgqSzmC2SY6XwoFkqbyTYLTYHs53q4IK/Z3YFiTTyPBAg6GpIA1JtaD+VF9QE0m9oLmqoFc6/rWBP4L3Ifpfo/dDop8y/Zno7/y3hkK2NLKpcm2dHCzkaOOGKzneyYDBRRwyZJExjwuaZNQlwzYX98mFg1x52KWLXvtaiTcDaPlDk6PEHAAAAABJRU5ErkJggg==');
        }
        .send-input {
            border: none;
            border-radius: 50px;
            width: 95%;
            height: 40px;
            color: #d3d9d4;
            background-color: #36393e;
            font-size: 18px;
            padding-left: 10px;
            margin-left: 4px;
            margin-right: 10px;
            outline: none;
        }
        footer {
            display: flex;
            align-items: center;
            padding: 6px;
            background-color: #2f3136;
        }
        #settingsButton {
            position: fixed;
            top: 8px;
            right: 8px;
            background-color: #2f3136;
            color: white;
            padding: 10px;
            border: none;
            height: 30px;
            width: 30px;
            border-radius: 50%;
            cursor: pointer;
            background-repeat: no-repeat;
            background-size: 60%;
            background-position: center;
            background-image: url('data:image/png;base64,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')
        }
        .rotate-center {
            animation: rotate-center 0.4s ease-in-out both;
        }
        @keyframes rotate-center {
            0% {
                transform: rotate(0);
            }
            100% {
                transform: rotate(180deg);
            }
        }
        #scrollDownButton {
            display: none;
            position: fixed;
            bottom: 60px;
            left: 20px;
            background-color: #20c20e;
            background-repeat: no-repeat;
            background-size: 60%;
            background-position: center;
            background-image: url('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAIAAAACACAYAAADDPmHLAAAACXBIWXMAADsOAAA7DgHMtqGDAAAAGXRFWHRTb2Z0d2FyZQB3d3cuaW5rc2NhcGUub3Jnm+48GgAABFhJREFUeJzt3c2LTXEcx/H3zGSy8WxlCgtKWYiiFLKYolgwKxb+AyVlb8GKLNXYipJC+AMokX+BnRXKU3kYoY7FmZOLOzPn/O7v4fv7nc+rzmIW99zvPZ/PPdM03e8FEREREREREREREREREREp0ITHcy0HtgAbgXHgs8dzC0wBW4FVwBfgV9px/tgPPATmgGrgeAVcANanGy1764GL1Ndy8NrOAQ+AfelGg0ngGn8PNux4A0wnmjFn08Bblr6+s8Cy2MONA3dbDNccP4CZ2ENmbIb6mrW9vneAsZgDnuswnErQTdfwm+NsrAFXAR8dBlQJluYafgW8B1bGGPKU44DN8Qs4EWPQzIwSfnOcjDHo7IhD6k7wPx/hV8DVGMM+8DCoSvCHr/Ar4F7XJx93GHjO4THDLANu0+9fBzPALfz9Gfet6wNcCvDC4TELmQCu0887ge/wAV56PNeC9uLndtXnXwc+b/uDx55YL+BpgOH7UoJQ4T+J+SK2A18DvIjS/0QMFf4csCPi6wDgOGFeTKl3glDh/6DOIokjwPclBtSdoNDwGyrB4ooOv6ESDNeL8Bsqwd96FX5DJaj1MvxG30vQ6/AbfS2Bwh/QtxIo/CH6UgKFv4jSS6DwWyi1BAq/g9JKoPAdlFIChT+C3Eug8D3ItQQK36PcSqDwA8ilBAo/IOslUPgRWC2Bwo/IWgkUfgJWSqDwE0pdAoXf0QbgNPXHnB5R7wu6AhzAfWtFqhJYDn8K2AVso17IldwkcIn/l0MNHs+BnY7nj10Ci+GbXRK1gvqjSG0uwFfgqOPzxCqBxfDNLokaA+63GOzfC+H6aZ/QJbAYvuklUcc7DOarBCFDsvbxNvNLop45DOfjwoS6E4QIP9Y7f/CIsiRqHfWtM9W7w3oJUoXfHMGXRO0eccCSS5A6/IoIS6IOehiyxBJYCL/CYUlUV1s9Ddoco/yzxkoJrIRfATcd52htAnjtceDmAuZ6J7AUfgWcd5ylk8ueh861BNbCr4i0JGot8C7A8DmVwGL4UZdEHcTGv20HxSqBxfC1JGpe6BJYDF9LoiLNpPAX0IcSKPwllFwChd9SiSVQ+B2VVAKF76iEEij8EVkswWHafdnVB+CQ43Mo/AEWS7AZuAH8HHLen9RfXrHJ8dwKfwiLJQBYQ31Rz8wfx4DVI5xP4S/Cagl8UfgtlFoChd9BaSVQ+A5KKYHCH0HuJVD4HuRaAoXvUW4lUPgB5FIChR+Q9RIo/AislkDhR2StBAo/ASslUPgJpS6BwjdAS6JES6Kk30uiZF4fl0TJP/q0JEoWkHoXgN75BlgvgcKPwGoJFH5E1kqg8BOwUgKFn1DqEih8A3JcEiWe5bQkSgLJYUmUBGZ5SZREYnFJlERmaUmUJGJhSZQklnJJlBiRYkmUGBNzSZQYFWNJlBi3mXBLorIU7csGjVlDvfJ+4/zPr4DHwKdE84iIiIiIiIiIiIiIiIiIiPj0G9OVmiFXVtG7AAAAAElFTkSuQmCC');
            border: none;
            cursor: pointer;
            height: 26px;
            width: 26px;
            border-radius: 50%;
        }
        #settingsModal {
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background-color: #00000080;
            justify-content: center;
            align-items: center;
        }
        .modal-content {
            background: linear-gradient(
                0deg,
                #2f3136 80%,
                #242529 100%
            );
            background-color: #2f3136;
            padding: 20px;
            border-radius: 10px;
            width: 300px;
            height: 460px;
        }
        .modal-content label {
            color: #d3d9d4;
            margin-left: 4px;
            font-size: 12px;
        }
        .modal-content input {
            width: 280px;
	        font-size: 16px;
            padding: 8px;
            border-radius: 8px;
            border: none;
            color: gray;
            outline: none;
            background-color: #36393e;
            margin-bottom: 8px;
        }
        .modal-content select {
            margin-left: 80px;
            width: 100px;
	        font-size: 16px;
            padding: 8px;
            border-radius: 8px;
            border: none;
            color: gray;
            outline: none;
            background-color: #36393e;
            margin-bottom: 8px;
        }
	    h2 {
            font-size: 20px;
        }
        .modal-content button {
	        font-size: 16px;
            padding: 8px;
            margin-left: 60px;
            width: 160px;
            background-color: #20c20e;
            color: #2f3136;
            border: none;
            border-radius: 8px;
            cursor: pointer;
        }
        .current-channel {
            margin-left: 4px;
            font-size: 16px;
            margin-bottom: 8px;
            color: #d3d9d4;
        }
        #status {
            top: 48px;
            left: 4px;
            right: 4px;
            text-align: center;
            display: none;
            border: none;
            border-radius: 40px;
            position: fixed;
            height: 10px;
            font-size: 8px;
            z-index: 1000;
        }
        #chat::-webkit-scrollbar {
            width: 4px;
        }
        #chat::-webkit-scrollbar-track {
            background: #55b6d400;
            border-radius: 10px;
        }
        #chat::-webkit-scrollbar-thumb {
            background: #2f3136;
            border-radius: 10px;
        }
        .input-container {
            display: flex;
            justify-content: space-between;
            padding: 10px;
        }
        .fadeIn {
            -webkit-animation-name: fadeIn;
            animation-name: fadeIn;
            -webkit-animation-duration: 1s;
            animation-duration: 1s;
            -webkit-animation-fill-mode: both;
            animation-fill-mode: both;
        }

        @-webkit-keyframes fadeIn {
            0% { opacity: 0; }
            100% { opacity: 1; }
        }

        @keyframes fadeIn {
            0% { opacity: 0; }
            100% { opacity: 1; }
        }

        .fadeOut {
            -webkit-animation-name: fadeOut;
            animation-name: fadeOut;
            -webkit-animation-duration: 1s;
            animation-duration: 1s;
            -webkit-animation-fill-mode: both;
            animation-fill-mode: both;
        }

        @-webkit-keyframes fadeOut {
            0% { opacity: 1; }
            100% { opacity: 0; }
        }

        @keyframes fadeOut {
            0% { opacity: 1; }
            100% { opacity: 0; }
        }
        @media (max-aspect-ratio: 1/1) {
            h1 {
                font-size: 60px;
                margin: 40px;
            }
            .message {
                padding: 12px;
                max-width: 86%;
                border-radius: 28px;
                font-size: 50px;
            }
            .message2 {
                padding: 12px;
                max-width: 86%;
                border-radius: 28px;
                font-size: 50px;
            }
            .send-button {
                height: 120px;
                width: 120px;
                background-position-x: 28px;
            }
            .send-input {
                border-radius: 80px;
                width: 84%;
                height: 120px;
                font-size: 50px;
                padding-left: 28px;
            }
            footer {
                padding: 8px;
            }
            #settingsButton {
                top: 14px;
                right: 12px;
                height: 120px;
                width: 120px;
            }
            .rotate-center {
                animation: rotate-center 0.4s ease-in-out both;
            }
            #scrollDownButton {
                bottom: 180px;
                left: 40px;
                height: 80px;
                width: 80px;
            }
            .modal-content {
                padding: 40px;
                border-radius: 20px;
                width: 800px;
                height: 1500px;
            }
            .modal-content label {
                font-size: 40px;
                margin-left: 18px;
            }
            .modal-content input {
                width: 680px;
                margin-left: 10px;
                font-size: 40px;
                padding: 40px;
                border-radius: 40px;
                margin-bottom: 40px;
            }
            .modal-content select {
                margin-left: 40px;
                width: 400px;
                font-size: 40px;
                padding: 40px;
                border-radius: 40px;
                margin-bottom: 20px;
            }
            h2 {
                font-size: 60px;
            }
            .modal-content button {
                font-size: 40px;
                padding: 20px;
                margin-left: 180px;
                width: 400px;
                border-radius: 40px;
            }
            .current-channel {
                margin-left: 18px;
                font-size: 40px;
                margin-bottom: 10px;
            }
            #status {
                top: 150px;
                left: 2px;
                right: 2px;
                border-radius: 20px;
                height: 40px;
                font-size: 32px;
            }
            #chat::-webkit-scrollbar {
                width: 6px;
            }
        }
    </style>
</head>
<body>
    <h1>Chat</h1>
    <div id="chat" onscroll="checkScrollPosition()"></div>
    <div id="status"></div>

    <button id="settingsButton"></button>
    <button id="scrollDownButton" onclick="scrollToBottom()"></button>

    <div id="settingsModal">
        <div class="modal-content">
            <h2>Ayarlar</h2>
            <label for="ssid">SSID:</label>
            <input type="text" id="ssid" placeholder="SSID girin">
            <label for="password">Password:</label>
            <input type="password" id="password" placeholder="Şifre girin">
            <label for="broadcast">SSID Broadcast:</label>
            <select id="broadcast">
                <option value="true">Açık</option>
                <option value="false">Kapalı</option>
            </select>
            <button onclick="saveWiFiSettings()">Wi-Fi Ayarlarını Kaydet</button>

            <h2>Kanal Ayarı</h2>
            <div class="current-channel">Mevcut Kanal: <span id="currentChannel"></span></div>
            <input type="number" id="channel" min="1" max="125" placeholder="Kanal girin">
            <button onclick="saveChannelSettings()">Kanal Ayarını Kaydet</button>
        </div>
    </div>

    <footer>
        <input class="send-input" type="text" id="message" oninput="toggleSendButton()">
        <button class="send-button" id="sendButton" onclick="sendMessage()" disabled></button>
    </footer>

    <script>
        var connection;
        var reconnectInterval = 1000;
        var userName = prompt("Lütfen kullanıcı adınızı girin:", "");

        function connectWebSocket() {
            connection = new WebSocket('ws://192.168.4.1:81/');
            console.log("WebSocket bağlantısı başlatıldı.");

            connection.onopen = function () {
                console.log('WebSocket bağlantısı açıldı!');
                showStatus("Bağlandı", "green");

                connection.send("GET_CHANNEL");
            };

            connection.onmessage = function (event) {
                console.log("Mesaj alındı:", event.data);

                if (event.data.startsWith("CHANNEL:")) {
                    var newChannel = event.data.split(":")[1];
                    document.getElementById('currentChannel').innerHTML = newChannel;
                    console.log("Mevcut kanal güncellendi: " + newChannel);
                } else {
                    var chat = document.getElementById('chat');
                    var userAtBottom = isUserAtBottom();

                    var firstColonIndex = event.data.indexOf(":");
                    var sender = event.data.substring(0, firstColonIndex);
                    var messageContent = event.data.substring(firstColonIndex + 1).trim();
                    console.log("Gönderen: " + sender);
                    console.log("Mesaj içeriği: " + messageContent);

                    var messageDiv = document.createElement('div');
                    messageDiv.className = 'message2';

                    if (window.matchMedia("(max-aspect-ratio: 1/1)").matches) {
                        messageDiv.innerHTML = "<strong style='color:lightblue; font-size:40px;'>" + sender + "</strong><br>" + messageContent;
                    } else {
                        messageDiv.innerHTML = "<strong style='color:lightblue; font-size:12px;'>" + sender + "</strong><br>" + messageContent;
                    }

                    chat.appendChild(messageDiv);

                    if (userAtBottom) {
                        scrollToBottom();
                    } else {
                        document.getElementById('scrollDownButton').style.display = 'block';
                    }
                }
            };

            connection.onclose = function () {
                console.log('Bağlantı kesildi. Yeniden bağlanmaya çalışılıyor...');
                showStatus("Bağlantı Kesildi", "red");
                setTimeout(connectWebSocket, reconnectInterval);
            };

            connection.onerror = function (error) {
                console.log('WebSocket Hatası:', error);
                connection.close();
            };
        }

        function sendMessage() {
            var message = document.getElementById('message').value;
            console.log("Gönderilen mesaj: ", message);
            if (message.trim()) {
                var fullMessage = userName + ": " + message;
                connection.send(fullMessage);

                var chat = document.getElementById('chat');
                var messageDiv = document.createElement('div');
                messageDiv.className = 'message';
                messageDiv.innerHTML = message; 
                chat.appendChild(messageDiv);

                document.getElementById('message').value = '';
                document.getElementById('message').focus();
                scrollToBottom();
                document.getElementById('scrollDownButton').style.display = 'none';
                toggleSendButton();
            }
        }

        function isUserAtBottom() {
            var chat = document.getElementById('chat');
            return chat.scrollHeight - chat.scrollTop <= chat.clientHeight + 1;
        }

        function scrollToBottom() {
            var chat = document.getElementById('chat');
            chat.scrollTop = chat.scrollHeight;
            document.getElementById('scrollDownButton').style.display = 'none';
        }

        function toggleSendButton() {
            var messageInput = document.getElementById('message').value.trim();
            var sendButton = document.getElementById('sendButton');

            if (messageInput.length > 0) {
                sendButton.disabled = false;
            } else {
                sendButton.disabled = true;
            }
        }

        function checkScrollPosition() {
            var chat = document.getElementById('chat');
            var scrollDownButton = document.getElementById('scrollDownButton');

            if (chat.scrollHeight - chat.scrollTop <= chat.clientHeight + 50) {
                scrollDownButton.style.display = 'none';
            }
        }

        window.onload = function () {
            connectWebSocket();
            scrollToBottom();

            var settingsButton = document.getElementById('settingsButton');
            var settingsModal = document.getElementById('settingsModal');

            settingsButton.onclick = function() {
                settingsButton.classList.add('rotate-center');
                setTimeout(function() {
                    settingsButton.classList.remove('rotate-center');
                }, 400);
    
                settingsModal.style.display = "flex";
            };

            window.onclick = function(event) {
                if (event.target === settingsModal) {
                    settingsModal.style.display = "none";
                }
            };
        }

        document.getElementById('message').addEventListener('keydown', function(event) {
            if (event.key === 'Enter') {
                event.preventDefault();
                sendMessage();
            }
        });

        function saveWiFiSettings() {
            var ssid = document.getElementById('ssid');
            var password = document.getElementById('password');
            var broadcast = document.getElementById('broadcast');

            if (!ssid.value) {
                ssid.style.border = "2px solid red";
            } else {
                ssid.style.border = "";
            }
    
            if (!password.value) {
                password.style.border = "2px solid red";
            } else {
                password.style.border = "";
            }
    
            if (!broadcast.value) {
                broadcast.style.border = "2px solid red";
            } else {
                broadcast.style.border = "";
            }

            if (!ssid.value || !password.value || !broadcast.value) {
                return;
            }

            var settings = {
                ssid: ssid.value,
                password: password.value,
                broadcast: broadcast.value
            };

            if (connection.readyState === WebSocket.OPEN) {
                connection.send("SETTINGS:" + JSON.stringify(settings));
                console.log("Wi-Fi Ayarları gönderildi: " + JSON.stringify(settings));
            } else {
                console.log("WebSocket bağlantısı kapalı, ayarlar gönderilemedi.");
            }

            document.getElementById('settingsModal').style.display = "none";
        }

        function saveChannelSettings() {
            var channel = document.getElementById('channel');

            if (!channel.value) {
                channel.style.border = "2px solid red";
            } else {
                channel.style.border = "";
            }

            if (!channel.value) {
                return;
            }

            if (connection.readyState === WebSocket.OPEN) {
                connection.send("CHANNEL:" + channel.value);
                console.log("Kanal Ayarı gönderildi: " + channel.value);

                document.getElementById('currentChannel').innerHTML = channel.value;
            } else {
                console.log("WebSocket bağlantısı kapalı, kanal ayarı gönderilemedi.");
            }

            document.getElementById('settingsModal').style.display = "none";
        }

        function showStatus(message, color) {
            var statusElement = document.getElementById('status');
            statusElement.innerHTML = message;
            statusElement.style.backgroundColor = color;
            statusElement.classList.remove('fadeOut');
            statusElement.classList.add('fadeIn');
            statusElement.style.display = "block";

            setTimeout(function () {
                statusElement.classList.remove('fadeIn');
                statusElement.classList.add('fadeOut');

                setTimeout(function() {
                    statusElement.style.display = "none";
                }, 1000);
            }, 3000);
        }
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
    server.send(200, "text/html", html);
    Serial.println("HTML arayüzü gönderildi.");
}