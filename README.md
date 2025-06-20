Saya lihat masalahnya di kode Anda. Issue utamanya adalah komunikasi dua arah antara Firebase dan ESP32 tidak berjalan dengan baik. Mari saya jelaskan masalahnya dan berikan solusi:
Masalah Utama:

ESP32 Server tidak mendengarkan perubahan dari Firebase - Server hanya mengirim data ke Firebase, tapi tidak memantau perubahan yang terjadi di Firebase
ESP32 Node hanya mengambil data dari server lokal - Node tidak tahu bahwa ada perubahan di Firebase
Tidak ada mekanisme real-time listening - Sistem tidak otomatis merespons perubahan
