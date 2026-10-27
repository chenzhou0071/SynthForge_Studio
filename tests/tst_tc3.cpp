#include <QtTest>

#include "core/cloud3d/Tc3Signature.h"

class Tc3Test : public QObject
{
    Q_OBJECT
private slots:
    void hmacMatchesRfcVector()
    {
        QCOMPARE(hmacSha256("key", "The quick brown fox jumps over the lazy dog").toHex(),
                 QByteArray("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));
    }

    void authorizationMatchesGoldenVector()
    {
        // 独立按 TC3 规范算出的基准：UTC 2023-11-14、hunyuan、payload {}
        const QString auth = tc3Authorization(
            "AKIDEXAMPLE", "SECRETKEY", "hunyuan", "hunyuan.tencentcloudapi.com",
            QByteArray("{}"), 1700000000);
        QCOMPARE(auth,
                 QStringLiteral(
                     "TC3-HMAC-SHA256 Credential=AKIDEXAMPLE/2023-11-14/hunyuan/tc3_request, "
                     "SignedHeaders=content-type;host, Signature="
                     "70547bd77866804cbdd650185a56878abf4d4fb4a327a4408c0aa3c096834567"));
    }

    void actionIsSignedWhenProvided()
    {
        // 官方示例做法：把 x-tc-action 一并纳入签名，价值是小写后的接口名
        const QString auth = tc3Authorization(
            "AKIDEXAMPLE", "SECRETKEY", "hunyuan", "hunyuan.tencentcloudapi.com",
            QByteArray("{}"), 1700000000, QStringLiteral("SubmitHunyuanTo3DJob"));
        QCOMPARE(auth,
                 QStringLiteral(
                     "TC3-HMAC-SHA256 Credential=AKIDEXAMPLE/2023-11-14/hunyuan/tc3_request, "
                     "SignedHeaders=content-type;host;x-tc-action, Signature="
                     "24e3930eff1a924a1b29dc34da3513aa429de43a34091687a57aed4885864531"));
    }

    void contentTypeIsPartOfSignature()
    {
        // 服务端按实际 Content-Type 复算，charset 有无必须与请求头一致
        const QString auth = tc3Authorization(
            "AKIDEXAMPLE", "SECRETKEY", "hunyuan", "hunyuan.tencentcloudapi.com",
            QByteArray("{}"), 1700000000, QString(), QByteArray("application/json"));
        QCOMPARE(auth,
                 QStringLiteral(
                     "TC3-HMAC-SHA256 Credential=AKIDEXAMPLE/2023-11-14/hunyuan/tc3_request, "
                     "SignedHeaders=content-type;host, Signature="
                     "0b0956fcb0cc21325aa960920564d8659972d23ca76697d197bcafafa86c6f72"));
    }

    void signatureIsDeterministic()
    {
        const QString a =
            tc3Authorization("id", "key", "hunyuan", "h.example", "{}", 1700000000);
        const QString b =
            tc3Authorization("id", "key", "hunyuan", "h.example", "{}", 1700000000);
        QCOMPARE(a, b);
    }
};

QTEST_MAIN(Tc3Test)
#include "tst_tc3.moc"
