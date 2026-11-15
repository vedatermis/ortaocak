VedatOnline {VERSION} - Oyun İstemcisi
=====================================

Başlatma
--------
1. Bu klasörü istediğiniz bir yere kopyalayın (ör. C:\Oyunlar\VedatOnline).
2. config\client.toml dosyasını bir metin düzenleyiciyle açın:
   - [server] host  : bağlanılacak sunucunun adresi (alan adı veya IP). Pakette yer tutucu
                      "{HOST}" yazılıdır; sunucu yöneticinizden aldığınız adresi yazın.
   - [server] port  : sunucunun portu (varsayılan 15001).
   - server_public_key : sunucunun açık anahtarı (64 onaltılık karakter). İstemci yalnızca bu
                      anahtarla imzalanmış sunucuya bağlanır; anahtar uyuşmazsa "sunucu
                      doğrulanamadı" iletisi çıkar. Anahtarı sunucu yöneticisinden alın
                      (vo_server.exe --gen-keys çıktısı).
3. vo_client.exe dosyasını çift tıklayarak başlatın.
4. Kullanıcı adı ve şifrenizle giriş yapın, karakterinizi yaratın ya da seçin, "Oyuna Gir" ile
   dünyaya girin.

Gereksinimler: Windows 10/11 (64 bit), DirectX 11 destekli ekran kartı. Ek kurulum gerekmez.
Oyun verileri assets\ klasöründedir; vo_client.exe ile aynı klasörde kalmalıdır.

Kontroller (dünya)
------------------
W / S  (veya Yukarı / Aşağı ok)    ileri / geri (kameraya göre)
A / D  (veya Sol / Sağ ok)         yana (kameraya göre)
Sol tık (zemine)                   oraya yürü / koş; W A S D iptal eder
Sağ tık basılı + fare              kamerayı döndür
Fare tekerleği                     yakınlaştır / uzaklaştır
Shift (basılı)                     yürü (varsayılan koşu)
R                                  koşu / yürüyüş varsayılanını değiştir
F3                                 bilgi panelini aç / kapat
Esc                                menü: Devam Et, Karakter Seçimine Dön, Oyundan Çık

Ayarlar
-------
config\client.toml içinde:
  [window] width / height / vsync     pencere boyutu ve dikey eşitleme
  [graphics] shadows = true|false      güneş gölgeleri
  [graphics] msaa = 1|2|4|8            kenar yumuşatma (1 = kapalı)
"Beni hatırla" yalnızca kullanıcı adını %LOCALAPPDATA%\VedatOnline\client_settings.toml
dosyasına kaydeder; şifre hiçbir yere kaydedilmez.

Sorun giderme
-------------
- Günlük dosyası: vo_client.log (vo_client.exe ile aynı klasör).
- "Sunucuya bağlanılamadı": host/port ayarını ve güvenlik duvarını kontrol edin.
- "Sunucu doğrulanamadı": server_public_key sunucunun anahtarıyla aynı olmalıdır.
- "Ekran aygıtı sıfırlandı": ekran kartı sürücüsü sıfırlandı; istemci her şeyi yeniden kurdu,
  tekrar giriş yapmanız yeterlidir.
