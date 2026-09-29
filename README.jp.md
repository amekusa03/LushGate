# LushGate (ラッシュゲート)

[English](README.md) | **日本語**

> ⚡ **ステータス: 実装完了・フィールドテスト中**  
> ファームウェア・回路・BLE Web UIの実装が完了し、現在フィールドテストを実施中です。

---

ESP32-C3 を用いた山間部・露地畑向けの自律型自動散水システム。  
降雨の有無（累積時間）をJ3Yトランジスタ増幅・低消費電力雨センサーで判断し、JQC-3F 3Vリレーモジュールで灯油ポンプを安全に制御して散水を行います。  
現場での設定・メンテナンス用に、**BLE (Bluetooth Low Energy) + Web Bluetooth UI** を搭載し、スマートフォンのブラウザから1タップで時刻補正、スリープ・散水設定のNV保存、散水履歴の確認やCSV出力が可能です。

---

## 1. 主な機能・特徴

- **超低消費電力運用 (Light Sleep)**:
  - 待機時はBluetoothを完全OFF (Light Sleep 約0.13mA)。数分おきに復帰し数msのパルス測定で雨量を積算。
- **オンデマンド Web Bluetooth 通信 (Web BLE)**:
  - ルーター不要。畑で `BOOTボタン` を押すだけで直接スマホのブラウザからBLE接続可能。
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
| **GPIO8** | `STATUS_LED` | Digital Output | 動作状態インジケータLED (BLEモード時点滅) |
| **GPIO9** | `USER_BUTTON` | Digital Input (Pull-up) | BOOTボタン共用 (2秒長押しでBLEモード起動) |
| **GPIO2-6, 10** | *(予備)* | GPIO / ADC1 | 予備・将来の拡張用（フロートスイッチ・水位センサ等） |
| **GPIO18/19**| `USB_D- / D+`| Native USB | ファームウェア書き込み・USBシリアルデバッグ |

---

## 3. 回路図 (Schematics)

### 全体ブロック図

```mermaid
graph TD
    BatteryESP[乾電池 単三×4本 (DC 6V)] -->|USB給電| ESP32[ESP32-C3 マイコン]
    
    ESP32 -->|GPIO0 (OUT) / GPIO1 (VCC)| RainSensor[J3Y増幅雨センサー]
    ESP32 -->|GPIO7 (IN) / 3.3V / GND| RelayModule[JQC-3F 3Vリレーモジュール]
    ESP32 -->|GPIO8| LED[状態表示LED]
    ESP32 -->|GPIO9| Button[BOOTボタン / AP起動]
    
    BatteryPump[乾電池 単三×2本 (DC 3V)] -->|接点 COM/NO| RelayModule --> Pump[灯油ポンプ 3V]
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
[ ESP32 制御系 (単三×4本 6V USB給電) ]   [ JQC-3F-03VDC-C 3Vリレーモジュール ]     [ ポンプ駆動系 (単三×2本 3V完全独立) ]
                                            +------------------+
ESP32 3.3V (給電) ------------------------> | VCC (3V/3.3V電源) |
GPIO7 (PUMP_CTRL 信号) -------------------> | IN  (制御信号入力)|
ESP32 GND (信号GND) -----------------------> | GND (電源GND)    |
                                            |                  |
                                            |    [ 接点端子 ]  |
                                            |         COM      | <──── 乾電池 (+) [単三×2本 3.0V]
                                            |         NO       | ────> [ 灯油ポンプ (+) ]
                                            +------------------+            │
                                                                           [ 灯油ポンプ (3V) ]
                                                                            │
                                                                       [ 灯油ポンプ (-) ]
                                                                            │
                                                                       乾電池 (-) [GND]
```
- **電源の完全独立（究極のノイズ対策）**:
  - **ESP32制御系**: **単三形乾電池4本（6V）** からUSB端子へ給電。
  - **ポンプ駆動系**: **単三形乾電池2本（3V）** から完全独立給電。
  - 電源元およびGNDが完全に物理的分離（アイソレーション）され、ポンプ回転時のモーターノイズ・突入電流・逆起電力によるマイコン誤動作リスクが根本的にゼロになります。
- **低電圧直結駆動**: リレーコイルが **3V（3.3V）系** のため、ESP32の 3.3V / GND / GPIO7 から直接シンプルに接続・駆動できます。

---

## 4. 運用モードとBLE操作ガイド

### 4.1 モード切り替え
- **通常運用モード (Normal Mode)**:
  - Light Sleepで設定周期（デフォルト3分）ごとに起動し、雨センサーをチェック・積算。
  - 朝の設定時刻（デフォルト07:00）に24h累積降雨が閾値（デフォルト60分）未満の場合、ポンプを自動デューティ駆動（3分ON / 2分OFF、正味10分）。
  - 散水完了・スキップ結果をNVS履歴に保存後、Light Sleepへ移行。
- **BLEモード (Web Bluetooth UI)**:
  - `BOOTボタン (GPIO9)` を押すとLEDが点滅し、BLEアドバタイジングが開始。
  - スマホのブラウザで `https://amekusa03.github.io/LushGate/` を開き、「**LushGate に接続**」をタップするとBLE接続が確立。Wi-Fi接続・ルーター不要。
  - クライアント切断またはBLE操作完了後、Light Sleepへ自動復帰。

### 4.2 Web Bluetooth UI 機能一覧
1. **ステータス表示 & 時刻同期**:
   - ESP32内蔵RTCの現在時刻、本日累積雨量、雨センサー電圧、ポンプ稼働状態をリアルタイム取得・表示。
   - 「📱 スマホ時刻と同期」ボタンで1タップRTC補正。
2. **設定変更 (NVS保存)**:
   - 散水時刻 (時:分)、雨監視周期 (分)、散水スキップ降雨閾値 (分)、雨滴判定ADC閾値 (mV)、ポンプON/OFF/総散水時間を変更してNVSへ即時保存。
3. **散水履歴閲覧**:
   - 過去60回分の散水実績（日時、降雨積算、散水可否、稼働秒数）をインデックス指定で取得・表示。
4. **手動散水コマンド**:
   - 任意秒数のポンプ手動駆動（`PUMP_CMD` キャラクタリスティック経由）、即時停止。

---

## 5. BLE GATT インターフェース仕様

**Service UUID**: `12340000-5678-1234-5678-000000000000`

| キャラクタリスティック | UUID下4桁 | プロパティ | 説明 |
|---|---|---|---|
| `CONFIG`   | `0001` | Read / Write | `lushgate_config_t` バイナリ（設定値の読み書き・NVS保存） |
| `TIMESYNC` | `0002` | Write        | UNIX Epoch を `uint32LE` で書き込み → RTC更新 |
| `PUMP_CMD` | `0003` | Write        | `0x00`=OFF, `0x01`=ON, `[0x02, sec_lo, sec_hi]`=指定秒ON |
| `STATUS`   | `0004` | Read / Notify| JSON文字列（雨量・ポンプ状態・時刻・センサー生値） |
| `HISTORY`  | `0005` | Read / Write | Write: インデックス `uint16LE` / Read: 当該エントリJSON |

---

## 6. ディレクトリ構成

```
LushGate/
├── README.md               # 英語版システム仕様書 & 取扱説明書
├── README.jp.md            # 日本語版システム仕様書 & 取扱説明書
├── LushGate_spec.md        # 原本仕様書
├── CMakeLists.txt          # ESP-IDF プロジェクトCMake
├── sdkconfig.defaults      # ESP-IDF デフォルト設定 (NimBLE / Light Sleep 等)
├── partitions.csv          # カスタムパーティションテーブル
├── docs/                   # GitHub Pages 公開ディレクトリ
│   ├── index.html          # Web Bluetooth PWA アプリ本体
│   ├── manifest.json       # PWA マニフェスト
│   ├── sw.js               # Service Worker (オフラインキャッシュ)
│   ├── icon-192.png        # PWA アプリアイコン (192px)
│   ├── icon-512.png        # PWA アプリアイコン (512px)
│   ├── qrcode.png          # アプリURL QRコード
│   ├── qrcode_print.png    # 印刷用 QRコード
│   ├── qrcode.svg          # SVG QRコード
│   ├── requirements_definition.html # 要件定義書 & システム構成図
│   └── qr/
│       ├── print_label_ble.html     # BLE版制御ボックス用ラベル印刷ページ
│       └── qr_ble_app.png           # BLE版アプリ URL QRコード
├── tools/
│   └── lushgate_ble_app.html        # BLE アプリ スタンドアロン版 (開発・配布用)
└── main/
    ├── CMakeLists.txt      # コンポーネントCMake
    ├── idf_component.yml   # NimBLE依存関係定義
    ├── main.c              # メイン制御フロー & Light Sleep / スケジュール管理
    ├── lushgate_pins.h     # GPIOピンアサイン定義
    ├── ble_gatt.c/.h       # BLE GATTサービス (NimBLE) & キャラクタリスティック定義
    ├── rain_sensor.c/.h    # 極性反転・低消費電力雨センサードライバ
    ├── pump_control.c/.h   # ポンプデューティ駆動 & 手動コマンドドライバ
    └── storage_manager.c/.h# NVS設定管理 & 履歴リングバッファ
```

---

## 7. ビルド & 書き込み手順 (ESP-IDF)

```bash
# ESP-IDF 環境をエクスポート
source ~/esp/esp-idf/export.sh

# ターゲット設定 (ESP32-C3)
idf.py set-target esp32c3

# ビルド
idf.py build

# 書き込み & シリアルモニタ
idf.py -p /dev/ttyACM0 flash monitor
```

---

## 🏷️ 制御ボックス用 QRコード・ラベル印刷 & GitHub Pages

屋外の制御盤・防水ボックスに貼れる QR コードとラベル印刷用 HTML を [docs/qr/](file:///home/kusa/ドキュメント/eSp32/LushGate/docs/qr) に用意しています。

* **[docs/qr/print_label_ble.html](file:///home/kusa/ドキュメント/eSp32/LushGate/docs/qr/print_label_ble.html)** : BLE版ラベル印刷ページ（ブラウザで開いて「印刷」を押すだけで防水シール作成可能）
* **Web アプリ公開URL**: `https://amekusa03.github.io/LushGate/`
* **オフライン・PWA対応**: 一度ブラウザで開いて「ホーム画面に追加」しておけば、電波の届かない山奥の畑（圏外）でも完全にスタンドアロン動作します。

### GitHub Pages の有効化手順
1. GitHub リポジトリ（`amekusa03/LushGate`）の **Settings** > **Pages** を開く。
2. **Build and deployment** の Source で **Deploy from a branch** を選択。
3. Branch を `main`、フォルダを `/docs` に設定して **Save** を押す。
4. 数分で `https://amekusa03.github.io/LushGate/` が公開されます。
