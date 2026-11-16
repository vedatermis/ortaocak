VedatOnline {VERSION} - Oyun Sunucusu
====================================

İçerik
------
vo_server.exe                     oyun sunucusu
vo_admin.exe                      yönetim aracı (hesap, karakter, eşya, veritabanı durumu)
gamedata\items.toml               eşya şablonları ve başlangıç donanımları (istemcidekiyle aynı olmalı)
gamedata\monsters.toml            canavarlar, doğma noktaları ve güvenli bölgeler (istemcidekiyle aynı olmalı)
gamedata\loot.toml                canavarların bıraktığı ganimet tabloları (istemcidekiyle aynı olmalı)
gamedata\skills.toml              yetenekler, yetenek yolları ve puan kuralları (istemcidekiyle aynı olmalı)
config\server.toml                varsayılan ayarlar (değiştirmeyin; üzerine yazmak için aşağıya bakın)
config\server.local.toml.example  makineye özel ayarlar ve gizli bilgiler için örnek
db\migrations\                    veritabanı şeması (sunucu açılışta otomatik uygular)
assets\terrain\zone1\             1. bölgenin arazisi (sunucu hareket doğrulaması için kullanır)

Gereksinimler: Windows 10/11 veya Windows Server (64 bit). Kalıcı veriler için PostgreSQL 14+
(veritabanı olmadan da çalışır; o zaman hesaplar ve karakterler bellekte tutulur ve sunucu
kapanınca kaybolur).

1. Sunucu anahtarı (bir kez)
----------------------------
Komut isteminde bu klasörde:

    vo_server.exe --gen-keys

Anahtar çifti config\keys\ klasörüne yazılır ve açık anahtar (64 onaltılık karakter) ekrana
basılır. Bu açık anahtarı oyunculara dağıtılan istemcinin config\client.toml dosyasındaki
server_public_key satırına yazın. config\keys\server_ed25519.key gizlidir: kimseyle paylaşmayın,
yedekleyin. Anahtarı değiştirirseniz (--gen-keys --force) tüm istemcilerin anahtarını da
güncellemeniz gerekir.

2. Veritabanı (PostgreSQL)
--------------------------
a) PostgreSQL'de oyun için bir kullanıcı ve veritabanı oluşturun, ör.:
       CREATE ROLE vedat LOGIN PASSWORD '...';
       CREATE DATABASE vedatonline OWNER vedat;
b) config\server.local.toml.example dosyasını config\server.local.toml adıyla kopyalayın ve
   [db] connection_string satırına bağlantı adresini yazın:
       connection_string = "postgresql://vedat:SIFRE@localhost:5432/vedatonline"
   Başka bir makinedeki veritabanı için sonuna ?sslmode=require ekleyin. Alternatif olarak
   VEDAT_DB_URL ortam değişkeni kullanılabilir (dosyadaki değerin önüne geçer).
c) Tablolar ilk açılışta otomatik oluşturulur. Sunucuyu başlatmadan uygulamak için:
       vo_admin.exe migrate
d) Durumu görmek için:  vo_admin.exe status
   Hesap oluşturmak için: vo_admin.exe account create <kullanıcı>   (şifreyi sorar)
   (Tüm komutlar: vo_admin.exe --help)

config\server.local.toml gizli bilgiler içerir; kimseyle paylaşmayın.

3. İlk başlatma
---------------
    vo_server.exe

Sunucu varsayılan olarak 15001 numaralı TCP portunu dinler (config\server.toml: [net]).
Windows Güvenlik Duvarı'nda bu port için gelen bağlantılara izin verin. Durdurmak için Ctrl+C:
sunucu herkese "sunucu kapanıyor" iletisini gönderir, oyuncuların konumlarını kaydeder ve kapanır.

Önemli ayarlar (config\server.local.toml içinde değiştirin):
  [net] listen_port, max_connections
  [server] maintenance = true    bakım modu: yeni bağlantılar reddedilir
  [auth] dev_auto_register       yalnızca geliştirme içindir; gerçek sunucuda false kalmalı
  [log] level, file              ör. file = "logs/server.log"

Çıkış kodları: 0 normal, 1 hatalı argüman, 2 ayar/anahtar hatası, 3 kütüphane hatası,
4 çalışma hatası, 5 veritabanı hatası, 6 dünya verisi (arazi) eksik/bozuk.
