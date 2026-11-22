# Pompa Otomatis Berbasis BLE Mesh dan MQTT

Tugas kelompok mata kuliah **Desain dan Aplikasi Internet of Things**.

| | |
|---|---|
| Dosen | Dr. Muhtadin, S.T., M.T. |
| Program studi | Teknik Komputer |

| NIM | Nama |
|---|---|
| 5024241016 | Yuke Brilliant Hestiavin |
| 5024241018 | Naufal Ahimsa Raushanfekr |
| 5024241072 | Rozaq Nafi'ul Hafidz |

## Tentang

Sensor DHT11 membaca suhu dan kelembapan. Data dikirim lewat BLE Mesh ke gateway. Saat suhu mencapai ambang, gateway menyalakan pompa dan mengirim data ke dashboard lewat MQTT.

**Status:** keempat node sudah lolos build dengan ESP-IDF v5.5.3. Pengujian di hardware masih berjalan, lihat tab Issues.

## Komponen

| Node | Folder | Fungsi | Board | Pin |
|---|---|---|---|---|
| 1 | `node1_pump_server` | Pompa lewat relay | ESP32-C3 Super Mini | GPIO 5 |
| 2 | `node2_sensor_lpn` | Sensor DHT11 (LPN) | ESP32-S3 N16R8 | GPIO 4 |
| 3 | `node3_relay_friend` | Relay dan friend | ESP32-S3 N16R8 | - |
| 4 | `node4_gateway_prov` | Provisioner dan gateway MQTT | ESP32-S3 N16R8 | - |
| - | `computer_dashboard` | Mosquitto dan dashboard | Komputer | Port 1883, 9001 |

Folder `common/` berisi konstanta dan format data yang dipakai semua node.

## Rangkaian

**Pompa (Node 1)**

- IN relay ke GPIO 5, VCC ke 5V, GND ke GND.
- Adaptor 12 V (+) ke COM relay. NO relay ke (+) pompa. (-) pompa ke (-) adaptor.
- GND adaptor disambung ke GND ESP32.
- Kalau relay aktif LOW, matikan "Relay aktif HIGH" di menuconfig.

**Sensor (Node 2)**

- VCC ke 3V3, DATA ke GPIO 4, GND ke GND.
- Modul DHT11 yang berbentuk papan biasanya sudah punya pull-up. Sensor dengan kaki terbuka perlu resistor 4.7k antara DATA dan VCC.

**Node 3 dan Node 4** cukup pakai USB 5V. Node 3 sebaiknya diletakkan di tengah, di antara Node 1, Node 2, dan Node 4.

Pin yang sebaiknya dihindari:
- C3 Super Mini: GPIO 2, 8, 9, 18, 19.
- S3 N16R8: GPIO 0, 3, 19, 20, 45, 46, dan GPIO 26 sampai 37 yang dipakai flash dan PSRAM.

## Cara menjalankan

**Yang perlu disiapkan:** ESP-IDF v5.5.x dan Docker Desktop.

### Build dan flash

Setiap node adalah project ESP-IDF sendiri. Contoh untuk Node 2:

```bash
cd node2_sensor_lpn
idf.py set-target esp32s3
idf.py menuconfig
idf.py build flash monitor -p /dev/cu.usbmodemXXXX
```

- Node 1 memakai `esp32c3`. Kalau flash gagal, tahan tombol BOOT, tekan RESET, lalu lepas BOOT.
- Ganti `/dev/cu.usbmodemXXXX` dengan port yang muncul di `ls /dev/cu.*`.
- Node 4 perlu SSID, password Wi-Fi, dan alamat broker (misal `mqtt://192.168.1.10:1883`) di menuconfig.

Pengaturan di menuconfig tersimpan di `sdkconfig`, yang tidak ikut di-commit.

### Dashboard

```bash
cd computer_dashboard
docker compose up -d
```

Buka `computer_dashboard/index.html` di browser. Dashboard terhubung ke `ws://localhost:9001`.

### Urutan demo

1. Nyalakan Mosquitto, lalu buka dashboard.
2. Nyalakan Node 1 dan Node 3.
3. Nyalakan Node 4. Node 4 otomatis men-provision dan mengonfigurasi Node 1 dan Node 3.
4. Nyalakan Node 2 terakhir, supaya friendship dengan Node 3 terbentuk setelah provisioning.

## Topik MQTT

| Topik | Arah | Isi |
|---|---|---|
| `iot/mesh/telemetry` | Node 4 ke dashboard | `{"temp": 31.0, "hum": 68.0, "pump": 1, "rssi": -62, "uptime": 120}` |
| `iot/mesh/cmd` | Dashboard ke Node 4 | `{"pump": 1}`, `{"pump": 0}`, `{"mode": "auto"}` |

`rssi` adalah sinyal Wi-Fi gateway, bukan BLE.

## Ambang pompa

- Default: pompa ON saat suhu 31 °C atau lebih, dan OFF saat di bawah 31 °C.
- Untuk histeresis, isi `PUMP_OFF_TEMP` di menuconfig Node 4 dengan 30. Pompa tidak akan bolak-balik di sekitar 31 °C.
- Perintah manual dari dashboard mematikan mode auto sampai `{"mode": "auto"}` dikirim.

## Catatan

- Node 2 tidak memakai deep sleep, karena deep sleep akan memutus friendship dengan Node 3.
- Node 3 hanya meneruskan paket. Node 3 tidak punya AppKey, jadi tidak bisa membaca isi perintah.

## Tes tanpa hardware

Dari root repo:

```bash
gcc -Wall -Icommon/include -Inode4_gateway_prov/main test/test_host.c -o test/test_host && ./test/test_host
```

## Kerja kelompok

Tugas dibagi lewat tab Issues. Langkahnya:

1. Clone repo, lalu atur nama dan email git pakai akun sendiri.
2. Buat branch per tugas, misalnya `node2-uji-dht11`.
3. Commit dengan akun sendiri, push, lalu buka PR ke `main`.
4. Minta satu anggota lain me-review sebelum merge.

Jangan commit password Wi-Fi.
