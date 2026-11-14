ANTI KEBALIK (mod AML 2.0.0, 32-bit)

BUILD (belum dikompilasi di sini - tidak ada NDK):
1. Taruh folder 'mod/' dari repo AML (RusJJ/AndroidModLoader) ke AntiKebalik/aml_headers/mod/
2. Jalankan: ndk-build   (butuh Android NDK)
3. Hasil: libs/armeabi-v7a/libAntiKebalik.so

PASANG:
- libAntiKebalik.so  -> folder mods/ (sejajar libCheatMenu.so)
- configs/AntiKebalik.ini -> folder configs/ (sejajar ARM.CheatMenu.ini)

PAKAI:
- Edit AntiKebalik.ini: AntiFlip = 1 (aktif) / 0 (mati). Berlaku dalam ~1 detik.
