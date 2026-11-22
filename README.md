# Pompa Otomatis Berbasis BLE Mesh dan MQTT

Sensor DHT11 membaca suhu dan kelembapan, lalu data dikirim lewat BLE Mesh ke gateway. Gateway menyalakan pompa saat suhu mencapai ambang dan mengirim telemetri ke MQTT. Dashboard menampilkan grafik dan status pompa secara real-time.

## Arsitektur

| Folder | Peran | Board |
|---|---|---|
| `node1_pump_server` | Pompa DC, Generic OnOff Server | ESP32-C3 Super Mini |
| `node2_sensor_lpn` | Sensor DHT11, Low Power Node | ESP32-S3 N16R8 |
| `node3_relay_friend` | Relay dan Friend | ESP32-S3 N16R8 |
| `node4_gateway_prov` | Provisioner, Proxy, gateway Wi-Fi ke MQTT | ESP32-S3 N16R8 |
| `computer_dashboard` | Mosquitto dan dashboard web | Komputer |
| `common` | Konstanta dan format payload, dipakai semua node lewat `EXTRA_COMPONENT_DIRS` | |

Node 1 dan Node 3 harus selalu menyala. Node 3 menjadi penghubung antara Node 1, Node 2, dan Node 4.

## Pin dan wiring

| Node | Board | Pin | Fungsi | Setting |
|---|---|---|---|---|
| 1 | ESP32-C3 Super Mini | GPIO 5 | IN modul relay | `PUMP_GPIO` |
| 2 | ESP32-S3 N16R8 | GPIO 4 | DATA DHT11 | `DHT11_GPIO` |
| 3 | ESP32-S3 N16R8 | - | Tidak ada, cukup 5V | |
| 4 | ESP32-S3 N16R8 | - | Tidak ada, cukup 5V | |

Pin bisa diubah lewat `menuconfig`. GPIO 5 dipakai karena ESP32-C3 hanya punya GPIO 0 sampai 21, jadi GPIO 26 dari brief tidak tersedia.

**Node 1, pompa (modul relay 5V):**

1. VCC relay ke 5V ESP32, GND relay ke GND ESP32, IN relay ke GPIO 5.
2. Adaptor 12 V (+) ke COM relay. NO relay ke (+) pompa. (-) pompa ke (-) adaptor 12 V.
3. GND adaptor 12 V disambung ke GND ESP32 supaya referensi ground sama.

Banyak modul relay aktif LOW. Jika pompa menyala saat boot atau tidak pernah menyala, buka `menuconfig` > Pompa, lalu matikan "Relay aktif HIGH".

Alternatif MOSFET logic-level (low side): (+) pompa ke 12 V, (-) pompa ke drain, source ke GND, gate ke GPIO 5 lewat resistor 220 Ω. Pasang dioda 1N4007 paralel pompa dengan katoda ke +12 V.

**Node 2, sensor DHT11:**

- VCC ke 3V3, DATA ke GPIO 4, GND ke GND.
- Modul DHT11 yang berbentuk board biasanya sudah punya pull-up. Sensor polos (kaki terbuka) butuh resistor 4.7k sampai 10k antara DATA dan VCC.

**Node 3 dan Node 4:** tidak ada sensor atau aktuator, cukup 5V dari USB. Node 3 diletakkan di tengah, di antara Node 1, Node 2, dan Node 4 sesuai gambar arsitektur, supaya sinyal BLE mesh sampai ke semua node.

**Pin yang sebaiknya tidak dipakai:**

- ESP32-C3 Super Mini: GPIO 2, 8, 9 (strapping; GPIO 9 biasanya tombol BOOT), dan GPIO 18, 19 (USB).
- ESP32-S3 N16R8: GPIO 0, 3, 45, 46 (strapping), GPIO 19, 20 (USB), dan GPIO 26 sampai 37 (dipakai flash dan PSRAM di dalam modul).

## Prasyarat

- ESP-IDF v5.5.x. Di tiap terminal, jalankan `. <lokasi-esp-idf>/export.sh` sebelum `idf.py`.
- Docker Desktop untuk Mosquitto.

## Build dan flash

Setiap node adalah project ESP-IDF sendiri. Contoh untuk Node 2:

```bash
cd node2_sensor_lpn
idf.py set-target esp32s3
idf.py menuconfig
idf.py build flash monitor -p /dev/cu.usbmodemXXXX
```

Node 1 memakai `esp32c3`. Jika flash gagal di C3 Super Mini, tahan tombol BOOT, tekan RESET, lalu lepas BOOT. Ganti `/dev/cu.usbmodemXXXX` dengan port yang muncul di `ls /dev/cu.*`.

Node 4 memerlukan SSID, password Wi-Fi, dan `GW_MQTT_URI` (IP komputer, misal `mqtt://192.168.1.10:1883`) di `menuconfig`. Nilai ini disimpan di `sdkconfig`, yang sudah masuk `.gitignore`.

## Dashboard

```bash
cd computer_dashboard
docker compose up -d
```

Buka `computer_dashboard/index.html` di browser. Dashboard terhubung ke `ws://localhost:9001`. Pastikan firewall komputer mengizinkan port 1883 dan 9001 dari jaringan Wi-Fi yang sama dengan ESP32.

## Urutan demo

1. Jalankan Mosquitto dan buka dashboard.
2. Nyalakan Node 1 dan Node 3.
3. Nyalakan Node 4. Dalam beberapa detik Node 4 men-provision dan mengonfigurasi Node 1 dan Node 3.
4. Nyalakan Node 2 terakhir, supaya friendship dengan Node 3 terbentuk setelah provisioning.

## Topik MQTT

| Topik | Arah | Isi |
|---|---|---|
| `iot/mesh/telemetry` | Node 4 ke dashboard | `{"temp": 31.0, "hum": 68.0, "pump": 1, "rssi": -62, "uptime": 120}` |
| `iot/mesh/cmd` | Dashboard ke Node 4 | `{"pump": 1}`, `{"pump": 0}`, `{"mode": "auto"}` |

`rssi` adalah kekuatan sinyal Wi-Fi gateway (Node 4), bukan BLE.

## Kontrol pompa

- Mode auto: pompa ON saat suhu >= `PUMP_ON_TEMP`, OFF saat suhu < `PUMP_OFF_TEMP`. Default keduanya 31 sesuai brief.
- Histeresis: isi `PUMP_OFF_TEMP` dengan nilai lebih kecil, misal 30, supaya pompa tidak bolak-balik di sekitar 31 °C.
- Manual: `{"pump": 1}` atau `{"pump": 0}` mematikan mode auto sampai `{"mode": "auto"}` dikirim.

## Tes tanpa hardware

Dari root repo:

```bash
gcc -Wall -Icommon/include -Inode4_gateway_prov/main test/test_host.c -o test/test_host && ./test/test_host
```

Tes ini memeriksa validasi payload dan logika histeresis pompa.

## Catatan teknis

- LPN tidak memakai deep sleep, karena deep sleep memutus friendship dengan Node 3. Radio hanya aktif saat poll, dan sensor dibaca setiap `SENSOR_PERIOD_S` detik.
- DHT11 hanya memberi bilangan bulat (suhu 1 °C, kelembapan 1 %), jadi payload cukup 2 byte.
- Node 3 tidak punya model aplikasi, jadi tidak ada AppKey yang di-bind. Node 3 hanya di-provision dan berperan sebagai relay dan friend.
- Node 4 menyimpan daftar node dan AppKey di NVS. Setelah reboot, node tidak perlu di-provision ulang.

## Kerja kelompok

Repo ini private. Tugas dibagi lewat tab Issues. Cara ngerjain:

1. Clone repo, lalu atur identitas git pakai akun sendiri:

   ```bash
   git clone https://github.com/yukebrillianth/Tugas_Kelompok_ESP.git
   git config user.name "Nama kamu"
   git config user.email "email-kamu@example.com"
   ```

2. Buat branch per tugas, misalnya `node2-uji-dht11`:

   ```bash
   git checkout -b node2-uji-dht11
   ```

3. Commit perubahan dengan pesan yang jelas, lalu push:

   ```bash
   git push -u origin node2-uji-dht11
   ```

4. Buka PR ke `main`. Minta satu anggota lain me-review sebelum merge.

Jangan commit kredensial Wi-Fi. Build dengan ESP-IDF v5.5.x supaya hasilnya sama dengan anggota lain.
