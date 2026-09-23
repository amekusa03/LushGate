# LushGate (ラッシュゲート)

[English](README.md) | **日本語**

> 🚧 **ステータス: 開発中（仕掛り / ハードウェア部品発注中）**  
> 本リポジトリは現在、部品発注およびプロトタイプ製作段階の仕掛り状態です。ファームウェア設計・回路設計・Web UIの先行実装が完了しています。

---

ESP32-C3 を用いた山間部・露地畑向けの自律型自動散水システム。  
降雨の有無（累積時間）をJ3Yトランジスタ増幅・低消費電力雨センサーで判断し、JQC-3F 3Vリレーモジュールで灯油ポンプを安全に制御して散水を行います。  
現場での設定・メンテナンス用に、**Wi-Fi APモード + Web UI** を搭載し、スマートフォンのブラウザから1タップで時刻補正、スリープ・散水設定のNV保存、散水履歴の確認やCSV出力が可能です。

---

## 1. 主な機能・特徴

- **超低消費電力運用 (Light Sleep)**:
  - 待機時はWi-Fi/Bluetoothを完全OFF。数分おきに復帰し数msのパルス測定で雨量を積算。
- **オンデマンド Wi-Fi AP (`http://192.168.4.1`)**:
  - ルーター不要。畑で `BOOTボタン` を長押しするだけで直接スマホから接続可能。
- **Webによる1タップ時刻補正 (RTC同期)**:
  - スマホのブラウザ時刻とワンタップでESP32内蔵RTCを同期。
- **Webによる動作・散水設定 (NVS保存)**:
  - 散水時刻、雨量スキップ閾値、雨監視スリープ間隔、ポンプデューティ駆動時間（ON/OFF/総時間）をWebから変更・NV（不揮発性メモリ）へ保存。
- **散水履歴のWeb参照 & CSV出力**:
  - 過去60回分の散水実績（日時、24h降雨積算、散水可否、実稼働時間）を一覧表示＆CSVダウンロード。
- **手動散水 & センサーテスト**:
  - 現場での配線確認用ポンプ手動駆動（安全タイマー付き）および雨センサーリアルタイム測定。

---

## 2. ハードウェア仕様 & I/O ピンアサイン

### マイコン: ESP32-C3 (3.3V Logic)

| GPIO | 機能名 | 入出力 / 属性 | 用途・接続先 |
|---|---|---|---|
| **GPIO0** | `PIN_RAIN_OUT` | ADC1_CH0 / Input | 雨センサー信号入力（J3Y増幅出力 OUT端子） |
| **GPIO1** | `PIN_RAIN_POWER`| Digital Output | 雨センサー給電パルス制御（VCC: 待機電力・腐食防止） |
| **GPIO7** | `PUMP_CTRL` | Digital Output | ポンプ駆動制御 (Active-High: JQC-3F 3Vリレーモジュール制御) |
| **GPIO8** | `STATUS_LED` | Digital Output | 動作状態インジケータLED (APモード時点滅) |
| **GPIO9** | `USER_BUTTON` | Digital Input (Pull-up) | BOOTボタン共用 (2秒長押しでWi-Fi AP起動) |
| **GPIO2-6, 10** | *(予備)* | GPIO / ADC1 | 予備・将来の拡張用（フロートスイッチ・水位センサ等） |
| **GPIO18/19**| `USB_D- / D+`| Native USB | ファームウェア書き込み・USBシリアルデバッグ |

---

## 3. 回路図 (Schematics)

### 全体ブロック図

```mermaid
graph TD
    SolarGen[ソーラー発電機 / コントローラ] -->|5V USB 出力| ESP32[ESP32-C3 マイコン]
    SolarGen -.->|12V 出力| Spare[予備 12V]
    
    ESP32 -->|GPIO0 (OUT) / GPIO1 (VCC)| RainSensor[J3Y増幅雨センサー]
    ESP32 -->|GPIO7 (IN) / 3.3V / GND| RelayModule[JQC-3F 3Vリレーモジュール]
    ESP32 -->|GPIO8| LED[状態表示LED]
    ESP32 -->|GPIO9| Button[BOOTボタン / AP起動]
    
    BatteryPump[乾電池 単一×2本 (DC 3V)] -->|接点 COM/NO| RelayModule --> Pump[灯油ポンプ 3V]
```

---

### (1) 雨センサー回路（J3Yトランジスタ電流増幅・パルス給電）

```
       [ + 端子 / VCC ] (ESP32 GPIO1: 3.3Vパルス給電)
           │
           ├─────────────────────────+
           │                         │ (Collector)
           ├──────────────+          │
           │              │          │
        [ 1kΩ ]        [ 100Ω ]      │
           │              │          │
        [ LED1 ]      [ 櫛形電極(+) ]│
        (通電表示)        : (雨水)   │
           │          [ 櫛形電極(-) ]│
           │              │          │
           │              │ (Base)   │
           │              +──────[ J3Y (NPN) ]
           │                         │ (Emitter)
           │                         ├──────────────> [ S 端子 / OUT ] ──> ESP32 GPIO0 (ADC1_CH0)
           │                         │
           │                      [ 100Ω ]
           │                         │
           ├─────────────────────────+
           │
       [ - 端子 / GND ] (ESP32 GND)
```
- **微小導通の増幅**: 水滴の付着による微小な導通電流を NPN トランジスタ（J3Y / S8050等）のベースに流し、エミッタ側の 100Ω 負荷抵抗に十分な電圧降下（$V_{OUT}$）を生じさせて確実に検出します。
- **低消費電力 & 腐食対策**: センサーの VCC（給電）を ESP32-C3 の **GPIO1** に接続し、サンプリング時のみ数ミリ秒パルス印加。非測定時は給電を停止して GPIO0/1 を完全 Hi-Z にすることにより、待機時消費電流ゼロおよび常時通電による電極の電気分解（腐食）を防止します。

---

### (2) ポンプ駆動回路（JQC-3F-03VDC-C 3Vリレーモジュール ＋ 乾電池完全独立給電）

```
[ ESP32 制御系 (ソーラー5V USB給電) ]   [ JQC-3F-03VDC-C 3Vリレーモジュール ]     [ ポンプ駆動系 (乾電池3V完全独立) ]
                                            +------------------+
ESP32 3.3V (給電) ------------------------> | VCC (3V/3.3V電源) |
GPIO7 (PUMP_CTRL 信号) -------------------> | IN  (制御信号入力)|
ESP32 GND (信号GND) -----------------------> | GND (電源GND)    |
                                            |                  |
                                            |    [ 接点端子 ]  |
                                            |         COM      | <──── 乾電池 (+) [単一×2本 3.0V]
                                            |         NO       | ────> [ 灯油ポンプ (+) ]
                                            +------------------+            │
                                                                           [ 灯油ポンプ (3V) ]
                                                                            │
                                                                       [ 灯油ポンプ (-) ]
                                                                            │
                                                                       乾電池 (-) [GND]
```
- **電源の完全独立（究極のノイズ対策）**:
  - **ESP32制御系**: ソーラー発電機の **5V USB出力** から給電（降圧コンバータ不要）。
  - **ポンプ駆動系**: **単一形乾電池2本（3V）** から完全独立給電。
  - 電源元およびGNDが完全に物理的分離（アイソレーション）され、ポンプ回転時のモーターノイズ・突入電流・逆起電力によるマイコン誤動作リスクが根本的にゼロになります。
- **低電圧直結駆動**: リレーコイルが **3V（3.3V）系** のため、ESP32の 3.3V / GND / GPIO7 から直接シンプルに接続・駆動できます。

---

## 4. 運用モードとWeb操作ガイド

### 4.1 モード切り替え
- **通常運用モード (Normal Mode)**:
  - Light Sleepで設定周期（デフォルト3分）ごとに起動し、雨センサーをチェック・積算。
  - 朝の設定時刻（デフォルト07:00）に24h累積降雨が閾値（デフォルト60分）未満の場合、ポンプを自動デューティ駆動（3分ON / 2分OFF、正味10分）。
  - 散水完了・スキップ結果をNVS履歴に保存後、Light Sleepへ移行。
- **APメンテナンスモード (Web UI)**:
  - `BOOTボタン (GPIO9)` を **2秒間長押し** するとLEDが点滅し、SoftAPとmDNSが起動。
  - スマホで Wi-Fi SSID: `LushGate-XXXX` に接続後、ブラウザで **`http://lushgate.local`** (または `http://192.168.4.1`) にアクセス。
  - 5分間無操作（または画面上の「スリープへ」ボタン押下）で自動停止しLight Sleepへ復帰。

### 4.2 Web UI 機能一覧
1. **ステータス & 時刻同期**:
   - ESP32内蔵RTCの現在時刻、本日累積雨量、雨センサー生ADC値・電圧、次回判定時刻を表示。
   - 「📱 スマホ時刻と同期」ボタンで1タップ補正。
2. **設定画面 (NV保存)**:
   - 散水時刻 (時:分)、雨監視周期 (分)、散水スキップ降雨閾値 (分)、雨滴判定ADC閾値 (mV)、ポンプON/OFF/総散水時間、APタイムアウトを変更してNVSへ即時保存。
3. **散水履歴**:
   - 過去60回分の散水実績（日時、降雨積算、散水可否、稼働秒数）を表示。
   - CSVファイルとして直接ダウンロード可能。
4. **手動テスト**:
   - 30秒間のポンプ試運転、即時停止、雨センサー即時測定。

---

## 5. Web REST API 仕様

| Method | URI | 説明 |
|---|---|---|
| `GET` | `/` | Web UI シングルページ (HTML/CSS/JS) |
| `GET` | `/api/status` | RTC時刻、雨量積算、センサ生値、設定値の取得 |
| `POST`| `/api/time` | 時刻同期 (`{"epoch": 1726904123}`) |
| `GET` | `/api/config` | 現在の設定値一覧取得 |
| `POST`| `/api/config` | 設定値の更新・NVS保存 |
| `GET` | `/api/history` | 散水履歴一覧 (JSON) |
| `GET` | `/api/history/csv` | 散水履歴のCSVダウンロード |
| `POST`| `/api/history/clear` | 散水履歴の全消去 |
| `POST`| `/api/pump/test` | ポンプ手動駆動 (`{"action":"start","duration_sec":30}`) |
| `POST`| `/api/system/sleep` | APモードを終了し即時Light Sleepへ移行 |

---

## 6. ディレクトリ構成

```
LushGate/
├── README.md               # 英語版システム仕様書 & 取扱説明書
├── README.jp.md            # 日本語版システム仕様書 & 取扱説明書
├── LushGate_spec.md        # 原本仕様書
├── CMakeLists.txt          # ESP-IDF プロジェクトCMake
└── main/
    ├── CMakeLists.txt      # コンポーネントCMake & Webファイル埋め込み
    ├── main.c              # メイン制御フロー & Light Sleep / スケジュール管理
    ├── lushgate_pins.h     # GPIOピンアサイン定義
    ├── rain_sensor.c/.h    # 極性反転・低消費電力雨センサードライバ
    ├── pump_control.c/.h   # ポンプデューティ駆動 & 手動テストドライバ
    ├── storage_manager.c/.h# NVS設定管理 & 履歴リングバッファ
    ├── wifi_ap.c/.h        # SoftAP 起動管理 (`http://192.168.4.1`)
    ├── web_server.c/.h     # HTTPサーバー & RESTful API 実装
    └── web/
        └── index.html      # モダンWeb UI (SPA / HTML5+CSS+JS)
```

---

## 7. ビルド & 書き込み手順 (ESP-IDF)

# ESP-IDF と ESP-Matter の環境をエクスポート
source /home/kusa/esp/esp-idf/export.sh
source /home/kusa/esp/esp-matter/export.sh

```bash
# ターゲット設定 (ESP32-C3)
idf.py set-target esp32c3

# ビルド
idf.py build

# 書き込み & シリアルモニタ
idf.py -p /dev/ttyACM0 flash monitor
```

---

## 🏷️ 制御ボックス用 QRコード・ラベル印刷

屋外の制御盤・防水ボックスに貼れる QR コードとラベル印刷用 HTML を [docs/qr/](file:///home/kusa/ドキュメント/eSp32/LushGate/docs/qr) に格納しています。

* [docs/qr/print_label.html](file:///home/kusa/ドキュメント/eSp32/LushGate/docs/qr/print_label.html) : ブラウザで開いて「印刷」を押すだけでラベルシールとして印刷可能
* [docs/qr/qr_web_url.png](file:///home/kusa/ドキュメント/eSp32/LushGate/docs/qr/qr_web_url.png) : Web設定画面 (`http://192.168.4.1`) QRコード
* [docs/qr/qr_wifi_lushgate.png](file:///home/kusa/ドキュメント/eSp32/LushGate/docs/qr/qr_wifi_lushgate.png) : Wi-Fi 自動接続用 QRコード
