#pragma once

#include <QString>

namespace Configs
{
    inline constexpr auto kOtpPlaceholder = "{otp}";

    // Advances and persists the counter for HOTP entries.
    QString ResolveOtpCode(int otpProfileId);

    QString SubstituteOtp(const QString &text, const QString &code);
}
