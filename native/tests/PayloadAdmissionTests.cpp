#include "vault/PayloadAdmission.h"
#include "vault/VaultFormat.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace nordssh::vault;
static constexpr std::string_view Empty = R"({"schema":1,"hosts":[],"groups":[],"credentials":[],"known_hosts":[],"snippets":[],"settings":{}})";
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static std::string settings(std::string_view members) {
    auto document=std::string(Empty);document.replace(document.size()-3,2,"{"+std::string(members)+"}");return document;
}
static void rejected(std::string_view input,const char* reason) {
    require(inspectPayloadStructure(input).status!=PayloadStatus::StructureAccepted,reason);
}
static void accepted(std::string_view input,const char* reason) {
    require(inspectPayloadStructure(input).status==PayloadStatus::StructureAccepted,reason);
}
static std::string integerArray(std::size_t count) {
    std::string result="[";
    for(std::size_t i=0;i<count;++i){if(i)result+=',';result+='0';}
    return result+"]";
}
int main(int argc,char** argv) {
    // Test-only stdin oracle interface; never installed or used for real vaults.
    if(argc==2&&std::string_view(argv[1])=="--synthetic-lines") {
#ifdef _WIN32
        if(_setmode(_fileno(stdin),_O_BINARY)==-1)return EXIT_FAILURE;
#endif
        std::string line;
        while(std::getline(std::cin,line)) {
            std::cout<<(inspectPayloadStructure(line).status==PayloadStatus::StructureAccepted?'1':'0')<<'\n';
        }
        return EXIT_SUCCESS;
    }
    try {
        const auto empty=inspectPayloadStructure(Empty);
        require(empty.emptySchemaAccepted()&&empty.nodes==8,"Empty schema root and node count");
        const auto unreviewed=inspectPayloadStructure(settings(R"("label":"test")"));
        require(unreviewed.status==PayloadStatus::StructureAccepted&&!unreviewed.emptySchemaAccepted()&&unreviewed.unreviewedRecords,"Structural admission must not authorize product records");
        for(const auto* input:{""," ","{}","[]","null","true","1",R"({"schema":1})","\xef\xbb\xbf{}"}) rejected(input,"Bad root/schema/BOM accepted");
        auto unknown=std::string(Empty);unknown.insert(unknown.size()-1,R"(,"unknown":0)");rejected(unknown,"Unknown root field");
        auto version=std::string(Empty);version.replace(version.find(":1"),2,":2");
        require(inspectPayloadStructure(version).status==PayloadStatus::UnsupportedSchema,"Unsupported version not rejected");
        for(const auto* input:{"1.0","1e0","01","+1","-","NaN","Infinity","truefalse","null0","0x10","1 2","--1","[1,]","{\"x\":1,}","/*x*/1","\"bad\\q\"","\"bad\nline\"","\"\\ud800\"","\"\\udc00\"","\"\\ud800\\u0041\""})
            rejected(settings(std::string(R"("value":)")+input),"Malformed JSON accepted");
        for(const auto* token:{"0","-0","9007199254740991","-9007199254740991","true","false","null",R"("\ud83d\ude00")",R"("\u0000\b\f\n\r\t\/\\\"")"})
            accepted(settings(std::string(R"("value":)")+token),"Valid bounded JSON scalar rejected");
        for(const auto* token:{"9007199254740992","-9007199254740992","99999999999999999999","-99999999999999999999"})
            rejected(settings(std::string(R"("value":)")+token),"Numeric bound bypassed");
        for(const auto* invalid:{"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xf5\x80\x80\x80","\x80","\xe2\x82"})
            rejected(settings(std::string(R"("value":")")+invalid+'"'),"Invalid UTF8 accepted");
        accepted(settings("\"value\":\"\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xf0\x9f\x98\x80\""),"UTF8 Cyrillic/astral rejected");
        for(const auto* members:{R"("x":0,"x":1)",R"("x":0,"\u0078":1)",R"("nested":{"x":0,"x":1})",R"("arr":[{"x":0,"x":1}])", "\"\xf0\x9f\x98\x80\":0,\"\\ud83d\\ude00\":1"})
            require(inspectPayloadStructure(settings(members)).status==PayloadStatus::DuplicateKey,"Decoded duplicate key bypassed");
        auto duplicate=std::string(Empty);duplicate.insert(1,R"("\u0073chema":1,)");rejected(duplicate,"Escaped root duplicate");
        auto escaped=std::string(Empty);escaped.replace(escaped.find("schema"),6,"\\u0073chema");
        require(inspectPayloadStructure(escaped).emptySchemaAccepted(),"Escaped equivalent root name");
        for(const auto* items:{"null","1","[]","\"text\""}) {
            auto document=std::string(Empty);document.replace(document.find("[]"),2,"["+std::string(items)+"]");rejected(document,"Root record array accepts nonobject");
        }
        auto records=std::string(Empty);records.replace(records.find("[]"),2,"[{}]");
        require(inspectPayloadStructure(records).unreviewedRecords&&!inspectPayloadStructure(records).emptySchemaAccepted(),"Nonempty object records not marked unreviewed");
        auto wrongType=std::string(Empty);wrongType.replace(wrongType.find("[]"),2,"{}");rejected(wrongType,"Wrong collection type");
        rejected(std::string(Empty)+"{}","Trailing root");
        rejected(std::string(Empty)+std::string(1,'\0'),"Raw trailing NUL");
        auto padded=std::string(Empty);padded.resize(MaxPlaintextBytes,' ');accepted(padded,"Exact payload byte bound");padded+=' ';rejected(padded,"Payload over bound");
        accepted(settings('"'+std::string(128,'k')+"\":0"),"Exact decoded key bound");
        rejected(settings('"'+std::string(129,'k')+"\":0"),"Key over bound");
        std::string escapedKey;
        for(int i=0;i<128;++i)escapedKey+="\\u006b";
        accepted(settings('"'+escapedKey+"\":0"),"Key limit must use decoded size");
        accepted(settings("\"x\":\""+std::string(1048576,'v')+'"'),"Exact decoded string bound");
        rejected(settings("\"x\":\""+std::string(1048577,'v')+'"'),"String over bound");
        accepted(settings("\"x\":"+integerArray(10000)),"Exact array bound");
        rejected(settings("\"x\":"+integerArray(10001)),"Array over bound");
        std::string members;
        for(int i=0;i<256;++i){if(i)members+=',';members+='"'+std::to_string(i)+"\":0";}
        accepted(settings(members),"Exact object member bound");rejected(settings(members+",\"extra\":0"),"Object over bound");
        accepted(settings("\"x\":"+std::string(14,'[')+'0'+std::string(14,']')),"Exact container depth bound");
        rejected(settings("\"x\":"+std::string(15,'[')+'0'+std::string(15,']')),"Container depth over bound");
        std::string nodes;
        for(int i=0;i<9;++i){if(i)nodes+=',';nodes+='"'+std::to_string(i)+"\":"+integerArray(10000);}
        auto exact=settings(nodes+",\"last\":"+integerArray(9982));
        const auto counted=inspectPayloadStructure(exact);
        require(counted.status==PayloadStatus::StructureAccepted&&counted.nodes==100000,"Exact total node bound");
        rejected(settings(nodes+",\"last\":"+integerArray(9983)),"Total node over bound");
        // Every prefix truncation must fail, and every possible byte mutation is
        // bounded. Mutation acceptance itself is NOT a grammar proof.
        for(std::size_t i=0;i<Empty.size();++i)rejected(Empty.substr(0,i),"Truncated schema accepted");
        for(std::size_t i=0;i<Empty.size();++i)for(unsigned b=0;b<256;++b){
            auto changed=std::string(Empty);changed[i]=static_cast<char>(b);static_cast<void>(inspectPayloadStructure(changed));
        }
        std::cout<<"Bounded payload grammar/root admission passed; record schemas and persistence remain unimplemented\n";
        return EXIT_SUCCESS;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return EXIT_FAILURE;}
}
