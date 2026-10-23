#include "vault/VaultCrypto.h"
#include <argon2.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace nordssh::vault;
static void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
template<class Action> static void reject(Action action, const char* message) {
    try { action(); } catch (const CryptoError&) { return; }
    throw std::runtime_error(message);
}
static std::vector<std::byte> hex(std::string_view value) {
    require(value.size()%2 == 0,"Bad fixture hex length");
    std::vector<std::byte> result;
    auto nibble = [](char c) -> unsigned {
        if (c>='0' && c<='9') return unsigned(c-'0');
        if (c>='a' && c<='f') return unsigned(c-'a'+10);
        throw std::runtime_error("Bad fixture hex digit");
    };
    for (std::size_t i=0; i<value.size(); i+=2) result.push_back(std::byte((nibble(value[i])<<4)|nibble(value[i+1])));
    return result;
}
static std::span<const std::byte> bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()),value.size()};
}
static bool equal(std::span<const std::byte> left, std::span<const std::byte> right) {
    return std::ranges::equal(left,right);
}
static void gcmVectors() {
    // OpenSSL's official evpciph_aes_common.txt, aes-256-gcm NIST vectors.
    std::array<std::byte,32> zeroKey{};
    std::array<std::byte,12> zeroNonce{};
    std::array<std::byte,16> zeroPlaintext{};
    const auto encrypted=encryptGcm(zeroKey,zeroNonce,zeroPlaintext,{});
    require(equal(encrypted.bytes,hex("cea7403d4d606b6e074ec5d3baf39d18")),"GCM ciphertext known answer");
    require(equal(encrypted.tag,hex("d0d1c8a799996bf0265b98b5d48ab919")),"GCM tag known answer");
    require(equal(decryptGcm(zeroKey,zeroNonce,encrypted.bytes,{},encrypted.tag).bytes(),zeroPlaintext),"GCM known answer decryption");
    const auto key=hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const auto nonce=hex("cafebabefacedbaddecaf888");
    const auto aad=hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    const auto plain=hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    const auto result=encryptGcm(KeyView(key.data(),32),NonceView(nonce.data(),12),plain,aad);
    require(equal(result.bytes,hex("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662")),"GCM AAD vector ciphertext");
    require(equal(result.tag,hex("76fc6ece0f4e1768cddf8853bb2d551b")),"GCM AAD vector tag");
    require(equal(decryptGcm(KeyView(key.data(),32),NonceView(nonce.data(),12),result.bytes,aad,result.tag).bytes(),plain),"GCM AAD vector decrypt");
    auto badTag=result.tag;
    badTag[0]^=std::byte{1};
    reject([&]{(void)decryptGcm(KeyView(key.data(),32),NonceView(nonce.data(),12),result.bytes,aad,badTag);},"Invalid GCM tag accepted");
    auto badAad=aad; badAad[0]^=std::byte{1};
    reject([&]{(void)decryptGcm(KeyView(key.data(),32),NonceView(nonce.data(),12),result.bytes,badAad,result.tag);},"Invalid AAD accepted");
    const auto empty=encryptGcm(zeroKey,zeroNonce,{},{});
    require(empty.bytes.empty() && decryptGcm(zeroKey,zeroNonce,{}, {},empty.tag).bytes().empty(),"Empty low-level GCM payload");
}
static void argonVector() {
    // RFC 9106 section 5.3, tests underlying linked Argon2id implementation.
    std::array<std::uint8_t,32> out{},password{};
    std::array<std::uint8_t,16> salt{};
    std::array<std::uint8_t,8> secret{};
    std::array<std::uint8_t,12> aad{};
    password.fill(1); salt.fill(2); secret.fill(3); aad.fill(4);
    argon2_context context{};
    context.out=out.data(); context.outlen=32;
    context.pwd=password.data(); context.pwdlen=32;
    context.salt=salt.data(); context.saltlen=16;
    context.secret=secret.data(); context.secretlen=8;
    context.ad=aad.data(); context.adlen=12;
    context.t_cost=3; context.m_cost=32; context.lanes=4; context.threads=4;
    context.version=0x13;
    require(argon2_ctx(&context,Argon2_id)==ARGON2_OK,"RFC Argon2id operation failed");
    require(equal(std::as_bytes(std::span(out)),hex("0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659")),"RFC Argon2id known answer");
    // Independent argon2-cffi 25.1.0 / bindings 26.1.0 reference output, profile
    // m=65536 KiB,t=3,p=1,version=19; synthetic password and salt 00..0f.
    std::array<std::byte,16> applicationSalt{};
    for (std::size_t i=0;i<applicationSalt.size();++i) applicationSalt[i]=std::byte(i);
    const auto applicationKey=derivePasswordKey("synthetic-password",applicationSalt);
    require(equal(applicationKey.bytes(),hex("4cf5d7f6f5227a142f47881077f2fe9dec43fa435ef4200f79fde60450ed7539")),"Application Argon2id profile reference");
}
static void envelopeTests() {
    constexpr std::string_view payload=R"({"schema":1,"hosts":[],"groups":[],"credentials":[],"known_hosts":[],"snippets":[],"settings":{}})";
    // Frozen output of reference/generate_envelope.py with cryptography 50.0.2,
    // argon2-cffi 25.1.0 / bindings 26.1.0. Public synthetic credentials only.
    const auto reference=hex(
        "4e4f5244564c540001000000a0000000000000000000000000112233445546778899aabbccddeeff01000000000102030405060708090a0b0c0d0e0f101112131415161718191a1b"
        "c001a558edb308d6db081247f1c6ad305dfeda105c2d4deda46802d834c75f4a34574b36e076383b1e37dad099f64887202122232425262728292a2b6100000000000000"
        "01000000000000000100000000000000000000008f4f0cc4a6f413a42d1fd192e737ae4a2debab73a890ff2e4662af6a4c4b2d49f39a49b11de0e443d09f39c959a039c1"
        "76ebee741d215b215fafaab558049ffe4aef4e274f6b90f1e2324588211dc943fda88b185a658346c6b6d0a593904b221e25e99da5dfbd8a341a78a63776653c6f");
    require(equal(openEnvelope(reference,"synthetic-password").payload.bytes(),bytes(payload)),"Independent Python envelope interoperability");
    const auto original=createEnvelope("synthetic-password",bytes(payload));
    auto opened=openEnvelope(original,"synthetic-password");
    require(equal(opened.payload.bytes(),bytes(payload)),"Envelope roundtrip");
    const KeyView key(opened.key.bytes().data(),32);
    require(equal(openEnvelopeWithKey(original,key).payload.bytes(),bytes(payload)),"Remembered-key authentication");
    std::array<std::byte,32> wrongKey{};
    reject([&]{(void)openEnvelopeWithKey(original,wrongKey);},"Wrong device key accepted");
    reject([&]{(void)openEnvelope(original,"wrong-password");},"Wrong password accepted");
    // Every file byte is authenticated, including wrapped-key metadata when
    // password derivation is bypassed by an explicitly supplied device key.
    for (std::size_t i=0; i<original.size(); ++i) {
        auto changed=original; changed[i]^=std::byte{1};
        reject([&]{(void)openEnvelopeWithKey(changed,key);},"Tampered envelope byte accepted");
    }
    for (const auto cut : {0U,1U,159U,160U,176U}) {
        reject([&]{(void)openEnvelopeWithKey(std::span(original).first(cut),key);},"Truncated envelope accepted");
    }
    auto extra=original; extra.push_back(std::byte{});
    reject([&]{(void)openEnvelopeWithKey(extra,key);},"Trailing bytes accepted");
    const auto saved=resealEnvelope(opened,bytes(payload));
    require(!std::equal(original.begin()+120,original.begin()+132,saved.begin()+120),"Save reused data nonce");
    require(std::equal(original.begin(),original.begin()+120,saved.begin()),"Ordinary save changed password wrapper");
    auto savedOpened=openEnvelope(saved,"synthetic-password");
    require(validateHeader(savedOpened.header,saved.size()).revision==2,"Save did not advance revision");
    require(equal(savedOpened.payload.bytes(),bytes(payload)),"Password opening after save");
    const auto independent=createEnvelope("synthetic-password",bytes(payload));
    require(!std::equal(original.begin()+24,original.begin()+40,independent.begin()+24),"Independent vault reused UUID");
    auto spliced=original;
    std::copy_n(independent.begin()+72,48,spliced.begin()+72);
    reject([&]{(void)openEnvelopeWithKey(spliced,key);},"Spliced password wrapper accepted");
    reject([&]{(void)createEnvelope("short",bytes(payload));},"Short password created envelope");
    reject([&]{(void)createEnvelope("synthetic-password",{});},"Empty envelope created");
    SecretBytes owner(32);
    auto moved=std::move(owner);
    require(owner.bytes().empty() && moved.bytes().size()==32,"Secret move left duplicate owner");
}
int main() {
    try {
        gcmVectors(); argonVector(); envelopeTests();
        std::cout<<"Crypto primitive and authenticated-envelope tests passed; JSON/persistence not tested\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return EXIT_FAILURE;
    }
}
