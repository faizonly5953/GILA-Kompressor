# GILA Kompresor

Kompresor file/folder dengan rasio ekstrem berbasis **context mixing** (keluarga PAQ/lpaq).
Sengaja lambat demi ukuran arsip sekecil mungkin. Ditulis dalam C++17, satu file source
(`gila.cpp`) untuk dua program: `gila-kompres` dan `gila-dekompres`.

Format arsip: `.gila`

## Daftar isi

- [Fitur](#fitur)
- [Isi repositori](#isi-repositori)
- [Cara pakai](#cara-pakai)
- [Level memori (`-m`)](#level-memori--m)
- [Performa dan kapan cocok dipakai](#performa-dan-kapan-cocok-dipakai)
- [Build dari source](#build-dari-source)
- [Cara kerja](#cara-kerja)
- [Format arsip `.gila`](#format-arsip-gila)
- [Keamanan dan integritas data](#keamanan-dan-integritas-data)
- [Kode keluar (exit code)](#kode-keluar-exit-code)
- [Pemecahan masalah](#pemecahan-masalah)
- [Batasan](#batasan)

## Fitur

- Rasio kompresi sangat tinggi untuk teks, CSV, log, kode, database, executable, dan data mentah.
- Mendukung **file dan folder** (struktur subfolder dipertahankan), banyak input sekaligus.
- **Drag & drop** di Windows: seret file/folder ke `gila-kompres.exe`, atau `.gila` ke `gila-dekompres.exe`.
- **Verifikasi CRC32** otomatis saat dekompres: hasil dijamin identik dengan aslinya, atau Anda diberi tahu.
- Nama file Unicode (UTF-8) didukung di Windows.
- Filter khusus executable (E8/E9) untuk `.exe`, `.dll`, ELF.
- File dalam folder diurutkan per ekstensi agar data sejenis berdekatan (rasio lebih baik).
- Hasil **deterministik dan identik di semua PC**: seluruh aritmetika model memakai integer, tanpa floating point.
- Progress dengan kecepatan dan perkiraan sisa waktu.

## Isi repositori

| File | Keterangan |
|------|------------|
| `gila.cpp` | Source code lengkap (C++17), satu file untuk kompresor dan dekompresor |
| `gila-kompres.exe` | Program kompres (Windows, sudah dibuild) |
| `gila-dekompres.exe` | Program dekompres (Windows, sudah dibuild) |
| `BACA-SAYA.txt` | Ringkasan cara pakai singkat |
| `README.md` | Dokumentasi ini |

## Cara pakai

### Drag & drop (paling mudah, Windows)

| Tujuan | Langkah | Hasil |
|--------|---------|-------|
| Kompres | Seret file atau folder ke ikon `gila-kompres.exe` | `<nama>.gila` di sebelah file/folder asli |
| Dekompres | Seret file `.gila` ke ikon `gila-dekompres.exe` | Diekstrak ke folder bernama sama di sebelah arsip |

Jika nama tujuan sudah ada, program tidak menimpa; ia membuat nama baru, mis. `data (2).gila` atau folder `data (2)`.
Jika dijalankan lewat drag & drop (jendela konsol milik program sendiri), jendela menunggu Enter sebelum menutup agar hasilnya sempat dibaca.

### Lewat command line

```
gila-kompres  [-m1..-m11] [-o arsip.gila] file_atau_folder [file_atau_folder ...]
gila-dekompres arsip.gila [folder_tujuan]
```

Contoh:

```
:: kompres satu folder dengan level default (-m8)
gila-kompres C:\data\laporan

:: kompres beberapa input sekaligus, level 10, nama output ditentukan
gila-kompres -m10 -o backup.gila C:\data\log.txt C:\data\database

:: dekompres ke folder tertentu
gila-dekompres backup.gila D:\hasil

:: opsi -o juga berlaku untuk dekompres
gila-dekompres backup.gila -o D:\hasil
```

Opsi:

| Opsi | Program | Arti |
|------|---------|------|
| `-m1` ... `-m11` | kompres | Level memori, default `-m8` |
| `-o <path>` | kompres | Nama/lokasi arsip output. Default: `<nama input pertama>.gila` di folder input |
| `-o <folder>` atau argumen kedua | dekompres | Folder tujuan ekstrak. Default: folder bernama sama dengan arsip |

Tanpa argumen, kedua program menampilkan petunjuk singkat.

## Level memori (`-m`)

Level menentukan ukuran tabel hash model. Makin tinggi, makin besar memori dan makin baik rasio **untuk data besar**.

| Level | RAM kira-kira | Catatan |
|-------|---------------|---------|
| `-m6` | ~0,2 GB | Aman untuk PC dengan RAM kecil |
| `-m8` | ~0,7 GB | **Default** |
| `-m10` | ~2,3 GB | Paling bermanfaat untuk data besar (> 100 MB) |
| `-m11` | ~4,3 GB | Maksimum |

Rumus pemakaian memori (dari source):

```
RAM = 2^(15+level) * 64  +  2^(19+min(level,9))  +  2^(16+min(level,9))  +  ~45 MB
        tabel hash          buffer riwayat          tabel match model       tabel lain
```

> **Penting:** dekompres butuh RAM **sebesar level yang dipakai saat kompres**. Level disimpan di header arsip, jadi tidak perlu diberikan lagi saat dekompres, tetapi PC tujuan harus punya RAM cukup.

Level lebih tinggi tidak membantu untuk data kecil; tabel besar hanya menambah waktu inisialisasi dan memori.
Jika muncul pesan "Memori tidak cukup", turunkan level (mis. `-m6`).

## Performa dan kapan cocok dipakai

**Kecepatan:** sekitar 0,1 - 0,2 MB/detik, baik kompres maupun dekompres (simetris), single-thread. Contoh: 100 MB butuh sekitar 10-15 menit. Ini disengaja; kompresor context mixing menukar waktu dengan rasio.

**Paling ampuh untuk:**

- Teks, CSV, log, source code
- Database dan dump data
- File `.exe` / `.dll` (berkat filter E8/E9)
- Data mentah/biner terstruktur (tabel berukuran record tetap)
- File Excel/Word lama (`.xls`, `.doc`)

**Hampir tidak mengecil** (data sudah terkompresi):

- Gambar/video/audio: JPG, PNG, MP4, MP3
- Arsip: ZIP, RAR, 7z
- PDF
- `.xlsx`, `.docx`, `.pptx` (isinya sebenarnya ZIP)

Untuk kelompok kedua ini, hasilnya kira-kira sama dengan aslinya dan waktu terbuang percuma.

## Build dari source

Syarat: compiler C++17 (g++/MinGW-w64, clang, atau MSVC). Di Windows, `windows.h` dan `shellapi.h` dipakai untuk argumen Unicode dan deteksi jendela konsol.

Contoh dengan g++ (MinGW-w64):

```
g++ -O2 -std=c++17 -DGILA_MODE_COMPRESS   gila.cpp -o gila-kompres.exe   -static -lshell32
g++ -O2 -std=c++17 -DGILA_MODE_DECOMPRESS gila.cpp -o gila-dekompres.exe -static -lshell32
```

Di Linux/macOS (tanpa `-lshell32`):

```
g++ -O2 -std=c++17 -DGILA_MODE_COMPRESS   gila.cpp -o gila-kompres
g++ -O2 -std=c++17 -DGILA_MODE_DECOMPRESS gila.cpp -o gila-dekompres
```

Catatan build:

- `-DGILA_MODE_COMPRESS` atau `-DGILA_MODE_DECOMPRESS` memilih program. Tanpa keduanya, default-nya kompres.
- `-DTUNE` membuat parameter model (`LR`, `LRF`, `LIM`, `AR`, `SH`, `W0`, `FW`) bisa diubah lewat environment variable, untuk eksperimen penyetelan. **Jangan** dipakai untuk build rilis: arsip hanya bisa dibuka oleh program dengan parameter yang sama persis.
- Kompresor dan dekompresor harus dibuild dari versi `gila.cpp` yang sama.
- Perintah build di atas adalah contoh umum; flag persis yang dipakai untuk `.exe` di repo ini tidak tercatat di source.

## Cara kerja

Setiap **bit** data diprediksi lalu dikodekan dengan arithmetic coder. Semakin akurat prediksinya, semakin sedikit bit yang dipakai.

1. **Banyak model konteks (18 konteks ber-hash + 2 tabel langsung).** Setiap model menebak bit berikutnya berdasarkan konteks berbeda:
   - order 2, 3, 4, 5, 6, 8, 12, 16 (byte sebelumnya)
   - model **kata** (hash kata saat ini, bigram, dan skip-word)
   - model **sparse** (melewati byte tertentu)
   - model **kolom/tabel** (byte di baris atas pada kolom yang sama, cocok untuk CSV/tabel)
   - model **indirect** (byte yang dulu mengikuti konteks order-1 / order-2)
   - model **record** (mendeteksi panjang record tetap dari jarak berulang suatu byte, lalu memakai byte pada record sebelumnya)
   - order 0 dan order 1 langsung
2. **Match model.** Mencari kemunculan sebelumnya dari 6+ byte terakhir lewat tabel hash, lalu memprediksi byte yang dulu mengikutinya (berguna untuk pengulangan panjang).
3. **Slot adaptif.** Tiap konteks menyimpan probabilitas 16-bit, hitungan, dan riwayat 8 bit terakhir. Laju belajar menurun sesuai hitungan (`1/(n+1,5)`), dan riwayat bit dipetakan lagi lewat StateMap untuk koreksi.
4. **Tiga neural mixer + satu mixer akhir.** Prediksi semua model (domain logit/`stretch`) dicampur dengan bobot yang dilatih online. Mixer dipilih oleh konteks berbeda: jumlah konteks yang ditemukan dan panjang match, bit parsial byte saat ini, serta byte terakhir dan posisi bit.
5. **Empat tahap APM/SSE.** Hasil mixer disaring tabel koreksi (konteks: bit parsial, order-1, status match, hash order-2), lalu dirata-ratakan dengan bobot tetap.
6. **Arithmetic coder 32-bit** dengan presisi probabilitas 16-bit.

Pra-pemrosesan:

- **Filter E8/E9** untuk executable (deteksi lewat header `MZ` atau `\x7FELF`, ukuran 512 B - 512 MB): alamat relatif instruksi CALL/JMP x86 diubah menjadi absolut agar target yang sama menghasilkan byte yang sama. Reversibel sempurna.
- **Pengurutan**: dalam folder, subfolder lebih dulu, lalu file dikelompokkan per ekstensi, lalu per nama.

Dekompresor menjalankan model yang sama persis dan membaca bit dari arithmetic decoder, sehingga prosesnya simetris (kecepatan sama).

## Format arsip `.gila`

```
Offset  Ukuran  Isi
0       4       Magic "GILA"
4       1       Versi format (saat ini 1)
5       1       Level memori (1..11)
6       ...     Aliran arithmetic-coded yang berisi, berurutan:
                  - Header: jumlah entri (u32), lalu tiap entri:
                      tipe (u8: 0 file, 1 folder, 2 executable),
                      panjang path (u32), path UTF-8 dengan pemisah '/',
                      ukuran (u64, tidak ada untuk folder)
                  - Isi semua file, berurutan sesuai daftar entri
                  - CRC32 seluruh isi file (u32)
```

Semua bilangan little-endian. Header dan data berada dalam satu aliran terkompresi yang sama, jadi header pun ikut dikompres.

## Keamanan dan integritas data

- **CRC32** dari seluruh data dihitung saat kompres dan dibandingkan saat dekompres. Jika berbeda, program mencetak peringatan dan keluar dengan kode 2.
- **Perlindungan path traversal**: saat ekstrak, path yang absolut, memuat `:` atau komponen `..` ditolak dan entri dilewati, sehingga arsip berbahaya tidak bisa menulis di luar folder tujuan.
- Header divalidasi (jumlah entri, panjang path, tipe) sehingga arsip rusak ditolak.
- Windows SmartScreen mungkin memperingatkan karena `.exe` tidak bertanda tangan digital. Klik **More info**, lalu **Run anyway**. Anda juga bisa membuild sendiri dari `gila.cpp`.

## Kode keluar (exit code)

| Kode | Arti |
|------|------|
| 0 | Sukses (dan CRC32 cocok saat dekompres) |
| 1 | Gagal: input tidak ditemukan, bukan arsip GILA yang valid, versi/level tidak didukung, arsip rusak, tidak bisa membuat/membuka file, atau memori tidak cukup |
| 2 | Dekompres selesai tetapi CRC32 tidak cocok (arsip rusak) |

## Pemecahan masalah

| Gejala | Penyebab / solusi |
|--------|-------------------|
| `Memori tidak cukup` | Level terlalu tinggi untuk RAM yang ada. Kompres ulang dengan level lebih kecil. Untuk dekompres, gunakan PC dengan RAM cukup (level sudah tetap di arsip). |
| `Bukan file .gila yang valid` | Magic tidak cocok; file bukan arsip GILA atau terpotong. |
| `Versi/level arsip tidak didukung` | Arsip dibuat oleh versi program yang berbeda. |
| `CRC32 TIDAK cocok` | Arsip rusak atau terpotong (mis. unduhan tidak lengkap). |
| `Tidak bisa menulis ... (dilewati)` | Path tujuan tidak bisa ditulis, atau path di arsip dianggap tidak aman. |
| Sangat lambat | Normal (~0,1-0,2 MB/detik). Gunakan level lebih kecil untuk data kecil, dan hindari memasukkan file yang sudah terkompresi. |
| Peringatan SmartScreen | Lihat bagian [Keamanan](#keamanan-dan-integritas-data). |

## Batasan

- Single-thread dan lambat; tidak cocok untuk data puluhan GB atau pemakaian yang butuh cepat.
- Tidak ada kompresi paralel, tidak ada ekstrak sebagian (harus dekompres dari awal), dan tidak ada enkripsi/password.
- Tidak menyimpan metadata (tanggal modifikasi, atribut, permission); hanya path dan isi.
- Folder kosong dipertahankan, tetapi symlink dan file khusus dilewati (hanya file reguler dan folder).
- File yang gagal dibuka saat kompres diisi nol (program memberi peringatan).
- Arsip tidak kompatibel antar versi format yang berbeda.
