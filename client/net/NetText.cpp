#include "client/net/NetText.h"

#include <format>

namespace vo::client::net {

namespace vn = vo::net;

namespace {
constexpr std::string_view kConnectTitle = "Bağlantı Hatası";
constexpr std::string_view kLoginTitle = "Giriş Başarısız";
constexpr std::string_view kDisconnectTitle = "Bağlantı Kesildi";
} // namespace

UserMessage connectFailureMessage(vn::ConnectResult result)
{
    switch (result)
    {
    case vn::ConnectResult::Ok: return {};
    case vn::ConnectResult::ResolveFailed:
        return {std::string(kConnectTitle),
                "Sunucu adresi çözümlenemedi. İnternet bağlantınızı ve sunucu adresini kontrol edin."};
    case vn::ConnectResult::ConnectFailed:
        return {"Sunucuya Ulaşılamadı",
                "Sunucuya ulaşılamadı. Sunucu kapalı olabilir; lütfen daha sonra tekrar deneyin."};
    case vn::ConnectResult::Timeout:
        return {"Zaman Aşımı", "Sunucu zamanında yanıt vermedi (zaman aşımı). Lütfen daha sonra tekrar deneyin."};
    case vn::ConnectResult::VersionMismatch:
        return {"Sürüm Uyumsuz", "Oyun sürümünüz sunucuyla uyumlu değil. Lütfen oyunu güncelleyin."};
    case vn::ConnectResult::ServerFull:
        return {"Sunucu Dolu", "Sunucu şu anda dolu. Lütfen biraz sonra tekrar deneyin."};
    case vn::ConnectResult::Maintenance:
        return {"Bakım", "Sunucu şu anda bakımda. Lütfen daha sonra tekrar deneyin."};
    case vn::ConnectResult::VerificationFailed:
        return {"Sunucu Doğrulanamadı",
                "Sunucu doğrulanamadı: sunucunun kimliği beklenen anahtarla eşleşmiyor. Güvenliğiniz için "
                "bağlantı kesildi."};
    case vn::ConnectResult::ProtocolError:
        return {std::string(kConnectTitle),
                "Sunucu beklenmeyen bir yanıt gönderdi. Lütfen daha sonra tekrar deneyin."};
    case vn::ConnectResult::Disconnected:
        return {std::string(kConnectTitle), "Bağlantı kurulurken koptu. Lütfen tekrar deneyin."};
    case vn::ConnectResult::Cancelled: return {std::string(kConnectTitle), "Bağlantı iptal edildi."};
    }
    return {std::string(kConnectTitle), "Sunucuya bağlanılamadı."};
}

UserMessage missingServerKeyMessage()
{
    return {"Yapılandırma Hatası",
            "İstemci yapılandırmasında geçerli bir sunucu anahtarı yok (server_public_key). Geliştirme "
            "ortamında tools/dev/setup-dev.ps1 betiğini çalıştırın."};
}

UserMessage loginFailureMessage(vn::LoginResult result)
{
    switch (result)
    {
    case vn::LoginResult::Ok: return {};
    case vn::LoginResult::InvalidCredentials:
        return {std::string(kLoginTitle), "Kullanıcı adı veya şifre hatalı."};
    case vn::LoginResult::Banned:
        return {"Hesap Engellendi",
                "Bu hesap engellenmiş. Ayrıntılar için destek ekibiyle iletişime geçin."};
    case vn::LoginResult::AlreadyLoggedIn:
        return {std::string(kLoginTitle), "Bu hesapla şu anda zaten oyunda bulunuluyor."};
    case vn::LoginResult::TooManyAttempts:
        return {"Çok Fazla Deneme",
                "Çok fazla başarısız giriş denemesi yapıldı. Lütfen bir dakika bekleyip tekrar deneyin."};
    case vn::LoginResult::ServerError:
        return {"Sunucu Hatası", "Giriş sırasında sunucuda bir hata oluştu. Lütfen daha sonra tekrar deneyin."};
    }
    return {std::string(kLoginTitle), "Giriş yapılamadı."};
}

UserMessage loginFailureMessage(std::uint8_t rawResult)
{
    if (rawResult <= vn::kLoginResultMax)
        return loginFailureMessage(static_cast<vn::LoginResult>(rawResult));
    return {std::string(kLoginTitle), std::format("Giriş yapılamadı (sunucu yanıtı {}).", rawResult)};
}

std::string charCreateResultMessage(vn::CharCreateResult result)
{
    switch (result)
    {
    case vn::CharCreateResult::Ok: return {};
    case vn::CharCreateResult::NameInvalid:
        return "Bu isim kurallara uymuyor. İsim 3–16 karakter olmalı, bir harfle başlamalı ve yalnızca harf "
               "ve rakam içermelidir.";
    case vn::CharCreateResult::NameTaken: return "Bu isim zaten kullanılıyor. Lütfen başka bir isim seçin.";
    case vn::CharCreateResult::NoFreeSlot:
        return "Boş karakter yuvanız kalmadı. Yeni bir karakter için önce mevcut bir karakteri silin.";
    case vn::CharCreateResult::InvalidRaceOrClass: return "Seçilen ırk ve sınıf birlikte kullanılamıyor.";
    case vn::CharCreateResult::InvalidAppearance: return "Seçilen yüz veya saç görünümü geçersiz.";
    case vn::CharCreateResult::ServerError:
        return "Karakter yaratılırken sunucuda bir hata oluştu. Lütfen tekrar deneyin.";
    }
    return "Karakter yaratılamadı.";
}

std::string charDeleteResultMessage(vn::CharDeleteResult result)
{
    switch (result)
    {
    case vn::CharDeleteResult::Ok: return {};
    case vn::CharDeleteResult::NotFound: return "Karakter bulunamadı; karakter listesi yenilendi.";
    case vn::CharDeleteResult::ServerError:
        return "Karakter silinirken sunucuda bir hata oluştu. Lütfen tekrar deneyin.";
    }
    return "Karakter silinemedi.";
}

std::string enterWorldResultMessage(vn::EnterWorldResult result)
{
    switch (result)
    {
    case vn::EnterWorldResult::Ok: return {};
    case vn::EnterWorldResult::NotFound: return "Karakter bulunamadı; karakter listesi yenilendi.";
    case vn::EnterWorldResult::ServerError:
        return "Dünyaya girilirken sunucuda bir hata oluştu. Lütfen tekrar deneyin.";
    }
    return "Dünyaya girilemedi.";
}

std::string leaveWorldResultMessage(std::uint8_t result)
{
    if (result == vn::kLeaveWorldOk)
        return {};
    return std::format("Karakter seçimine dönülemedi (sunucu yanıtı {}).", result);
}

std::string itemMoveResultMessage(vn::ItemMoveResult result)
{
    switch (result)
    {
    case vn::ItemMoveResult::Ok: return {};
    case vn::ItemMoveResult::NoItem: return "Orada taşınacak bir eşya yok.";
    case vn::ItemMoveResult::BadSlot: return "Eşya oraya konamaz.";
    case vn::ItemMoveResult::WrongSlot: return "Bu eşya o yuvaya takılamaz.";
    case vn::ItemMoveResult::WrongClass: return "Sınıfın bu eşyayı kullanamaz.";
    case vn::ItemMoveResult::LevelTooLow: return "Seviyen bu eşya için yetersiz.";
    case vn::ItemMoveResult::SlotBlocked: return "İki elli silah takılıyken sol ele bir şey takılamaz.";
    case vn::ItemMoveResult::BagFull: return "Çantada yer yok.";
    case vn::ItemMoveResult::ServerError: return "Sunucu hatası: eşya taşınamadı.";
    }
    return std::format("Eşya taşınamadı (sunucu yanıtı {}).", static_cast<int>(result));
}

UserMessage disconnectMessage(const DisconnectInfo& info)
{
    const std::string title(kDisconnectTitle);
    switch (info.cause)
    {
    case DisconnectCause::RequestTimeout:
        return {title, "Sunucu isteğe zamanında yanıt vermedi (zaman aşımı); bağlantı kesildi."};
    case DisconnectCause::PingTimeout:
        return {title, "Sunucudan uzun süredir yanıt alınamadı (zaman aşımı); bağlantı kesildi."};
    case DisconnectCause::Closed: break;
    }

    if (info.serverError)
    {
        switch (static_cast<vn::ErrorCode>(*info.serverError))
        {
        case vn::ErrorCode::KickedDuplicateLogin:
            return {title, "Hesabınıza başka bir yerden giriş yapıldı; bu oturum kapatıldı."};
        case vn::ErrorCode::ServerShutdown:
            return {"Sunucu Kapanıyor", "Sunucu kapatılıyor. Lütfen daha sonra tekrar bağlanın."};
        case vn::ErrorCode::Timeout:
            return {title, "Uzun süre yanıt alınamadığı için sunucu bağlantıyı kapattı (zaman aşımı)."};
        case vn::ErrorCode::Flood:
            return {title, "Çok kısa sürede çok fazla istek gönderildiği için sunucu bağlantıyı kesti."};
        case vn::ErrorCode::ProtocolViolation:
        case vn::ErrorCode::BadState:
            return {title, "İstemci ile sunucu arasında bir iletişim hatası oluştu; bağlantı kesildi."};
        case vn::ErrorCode::InternalError:
            return {title, "Sunucuda bir hata oluştu; bağlantı kesildi. Lütfen tekrar giriş yapın."};
        }
        return {title, std::format("Sunucu bağlantıyı kesti (hata kodu {}).", *info.serverError)};
    }

    switch (info.closeReason)
    {
    case vn::CloseReason::PeerClosed: return {title, "Sunucu bağlantıyı kapattı."};
    case vn::CloseReason::NetworkError: return {title, "Sunucuyla bağlantı koptu."};
    case vn::CloseReason::BadFrame:
    case vn::CloseReason::DecryptFailed:
        return {title, "Sunucudan bozuk veri alındı; güvenliğiniz için bağlantı kesildi."};
    case vn::CloseReason::OutboundOverflow:
        return {title, "Ağ bağlantısı veri gönderimine yetişemedi; bağlantı kesildi."};
    case vn::CloseReason::LocalClose: return {title, "Bağlantı kapatıldı."};
    }
    return {title, "Sunucuyla bağlantı kesildi."};
}

} // namespace vo::client::net
