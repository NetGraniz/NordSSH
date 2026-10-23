#include "KeyMaterial.h"
#include "Session.h"
#include "vault/VaultCrypto.h"
#include <cstring>
#include <bit>
#include <openssl/evp.h>

namespace nordssh::ssh {
namespace {
bool whitespace(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; }
bool base64(char c) { return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'; }
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::uint32_t number() {
        if(bytes_.size()<4)throw SessionError();
        std::uint32_t value=0;
        for(int i=0;i<4;++i)value=(value<<8)|std::to_integer<unsigned char>(bytes_[static_cast<std::size_t>(i)]);
        bytes_=bytes_.subspan(4);return value;
    }
    std::span<const std::byte> string() {
        const auto length=number();if(length>bytes_.size())throw SessionError();
        const auto value=bytes_.first(length);bytes_=bytes_.subspan(length);return value;
    }
    bool empty() const {return bytes_.empty();}
private:
    std::span<const std::byte> bytes_;
};
std::string_view text(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()),value.size()};
}
std::span<const std::byte> positiveMpint(std::span<const std::byte> value) {
    if(value.empty()||(std::to_integer<unsigned>(value.front())&0x80)!=0)throw SessionError();
    if(value.front()==std::byte{}) {
        value=value.subspan(1);
        if(value.empty()||(std::to_integer<unsigned>(value.front())&0x80)==0)throw SessionError();
    }
    return value;
}
void publicShape(std::span<const std::byte> blob) {
    Reader reader(blob);const auto type=text(reader.string());
    if(type=="ssh-ed25519") {if(reader.string().size()!=32)throw SessionError();}
    else if(type=="ssh-rsa") {
        const auto exponent=positiveMpint(reader.string()),modulus=positiveMpint(reader.string());
        if(exponent.size()>4||modulus.size()<256||modulus.size()>1024)throw SessionError();
        const auto bits=(modulus.size()-1)*8+std::bit_width(std::to_integer<unsigned>(modulus.front()));
        if(bits<2048||bits>8192)throw SessionError();
        std::uint64_t e=0;for(auto byte:exponent)e=(e<<8)|std::to_integer<unsigned>(byte);
        if(e<3||(e&1)==0)throw SessionError();
    } else {
        const auto curve=text(reader.string());const auto point=reader.string();
        std::size_t size=0;
        if(type=="ecdsa-sha2-nistp256"&&curve=="nistp256")size=65;
        if(type=="ecdsa-sha2-nistp384"&&curve=="nistp384")size=97;
        if(type=="ecdsa-sha2-nistp521"&&curve=="nistp521")size=133;
        if(size==0||point.size()!=size||point.front()!=std::byte{4})throw SessionError();
    }
    if(!reader.empty())throw SessionError();
}
}
std::array<std::byte,32> admitOpenSshKey(std::string_view privateKey) {
    constexpr std::string_view begin="-----BEGIN OPENSSH PRIVATE KEY-----",end="-----END OPENSSH PRIVATE KEY-----";
    if(privateKey.empty()||privateKey.size()>1048576||privateKey.find('\0')!=std::string_view::npos)throw SessionError();
    while(!privateKey.empty()&&whitespace(privateKey.front()))privateKey.remove_prefix(1);
    while(!privateKey.empty()&&whitespace(privateKey.back()))privateKey.remove_suffix(1);
    if(!privateKey.starts_with(begin)||!privateKey.ends_with(end))throw SessionError();
    privateKey.remove_prefix(begin.size());privateKey.remove_suffix(end.size());
    vault::SecretBytes encoded(privateKey.size());
    auto* chars=reinterpret_cast<char*>(encoded.bytes().data());
    std::size_t count=0,padding=0;
    for(const char c:privateKey) {
        if(whitespace(c))continue;
        if(c=='=') {if(++padding>2)throw SessionError();}
        else if(!base64(c)||padding!=0)throw SessionError();
        chars[count++]=c;
    }
    if(count==0||count%4!=0)throw SessionError();
    vault::SecretBytes decoded(count/4*3);
    const auto length=EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.bytes().data()),
        reinterpret_cast<const unsigned char*>(chars),static_cast<int>(count));
    if(length<0||static_cast<std::size_t>(length)<padding)throw SessionError();
    auto bytes=decoded.bytes().first(static_cast<std::size_t>(length)-padding);
    constexpr std::string_view magic{"openssh-key-v1\0",15};
    if(bytes.size()<magic.size()||std::memcmp(bytes.data(),magic.data(),magic.size())!=0)throw SessionError();
    Reader reader(bytes.subspan(magic.size()));
    const auto cipher=text(reader.string()),kdf=text(reader.string());
    const auto options=reader.string();
    if(cipher=="none") {if(kdf!="none"||!options.empty())throw SessionError();}
    else if(cipher=="aes256-ctr"&&kdf=="bcrypt") {
        Reader costs(options);const auto salt=costs.string();const auto rounds=costs.number();
        if(salt.size()<16||salt.size()>64||rounds<1||rounds>64||!costs.empty())throw SessionError();
    } else throw SessionError();
    if(reader.number()!=1)throw SessionError();
    const auto publicKey=reader.string(),secret=reader.string();
    if(publicKey.empty()||publicKey.size()>16384||secret.empty()||secret.size()>16384||!reader.empty())throw SessionError();
    publicShape(publicKey);
    std::array<std::byte,32> hash{};unsigned int hashLength=0;
    if(EVP_Digest(publicKey.data(),publicKey.size(),reinterpret_cast<unsigned char*>(hash.data()),&hashLength,EVP_sha256(),nullptr)!=1||hashLength!=hash.size())throw SessionError();
    return hash;
}
}
