#include "mergeodf.h"

#include <sys/wait.h>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <Poco/RegularExpression.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Delegate.h>
#include <Poco/Zip/Compress.h>
#include <Poco/Zip/Decompress.h>
#include <Poco/Glob.h>
#include <Poco/TemporaryFile.h>
#include <Poco/Format.h>
#include <Poco/StringTokenizer.h>
#include <Poco/StreamCopier.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/Document.h>
#include <Poco/SAX/InputSource.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/DOM/Text.h>
#include <Poco/XML/XMLWriter.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Base64Decoder.h>
#include <Poco/Util/Application.h>
#include <Poco/DOM/DOMException.h>
#include <Poco/Util/Application.h>


using Poco::Net::HTMLForm;
using Poco::Net::MessageHeader;
using Poco::Net::PartHandler;
using Poco::Net::HTTPResponse;
using Poco::RegularExpression;
using Poco::Zip::Compress;
using Poco::Zip::Decompress;
using Poco::Path;
using Poco::File;
using Poco::TemporaryFile;
using Poco::StreamCopier;
using Poco::StringTokenizer;
using Poco::XML::DOMParser;
using Poco::XML::DOMWriter;
using Poco::XML::Element;
using Poco::XML::InputSource;
using Poco::XML::NodeList;
using Poco::XML::Node;;
using Poco::Dynamic::Var;
using Poco::JSON::Object;
using Poco::DynamicStruct;
using Poco::JSON::Array;
using Poco::Util::Application;
using Poco::XML::XMLReader;
using namespace Poco::Data::Keywords;
using Poco::Data::Statement;
using Poco::Data::RecordSet;
using Poco::Data::Session;

const std::string resturl = "/lool/merge-to/";
const int tokenOpts = StringTokenizer::TOK_IGNORE_EMPTY |
StringTokenizer::TOK_TRIM;

extern "C" MergeODF* create_object()
{
    return new MergeODF;
}

LogDB::LogDB()
{
    Poco::Data::SQLite::Connector::registerConnector();
}
LogDB::~LogDB() {
    Poco::Data::SQLite::Connector::unregisterConnector();
}

/// 從設定檔取得資料庫檔案位置名稱 & timeout
void LogDB::setDbPath()
{
    // 如果沒有跑系統，會讀取專案底下的 runTimeData/mergeodf.sqlite 來確保程式執行
#if ENABLE_DEBUG
    dbfile = "./runTimeData/mergeodf.sqlite";
#else
    const auto& app = Poco::Util::Application::instance();
    dbfile = app.config().getString("mergeodf.db_path", "./runTimeData/mergeodf.sqlite");
#endif
    std::cout<<"mergeodf: setDbPath: db: "<<dbfile<<std::endl;
}

/// 寫入 log: api+status+timestamp
/// status=狀態文字
/// @return string endpoint
void LogDB::notice(std::weak_ptr<StreamSocket> _socket,
        Poco::Net::HTTPResponse& response,
        std::string status)
{
    try
    {
        Session session("SQLite", dbfile);
        Statement insert(session);
        insert << "INSERT INTO access (api, status, ts) VALUES (?, ?, strftime('%s', 'now'))",
               use(api), use(status), now;
        session.close();
    }
    catch (Poco::Exception& e)
    {
        std::cerr << e.displayText() << std::endl;

        auto socket = _socket.lock();
        response.setStatusAndReason(
                HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
                "cannot log to database. message: " + status);
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        _exit(Application::EXIT_SOFTWARE);
        return;
    }
}

/// 傳回某個　api 的呼叫次數
int LogDB::getAccessTimes()
{
    int access = 0;
    Session session("SQLite", dbfile);
    Statement select(session);
    select << "select count(*) FROM access where api=? and status='start'",
           into(access), use(api);
    while (!select.done())
    {
        select.execute();
        break;
    }
    session.close();
    return access;
}

/// check if number
bool isNumber(std::string s)
{
    std::size_t char_pos(0);
    // skip the whilespaces
    char_pos = s.find_first_not_of(' ');
    if (char_pos == s.size())
        return false;
    // check the significand
    if (s[char_pos] == '+' || s[char_pos] == '-')
        ++char_pos; // skip the sign if exist
    int n_nm, n_pt;
    for (n_nm = 0, n_pt = 0;
            std::isdigit(s[char_pos]) || s[char_pos] == '.';
            ++char_pos)
    {
        s[char_pos] == '.' ? ++n_pt : ++n_nm;
    }
    if (n_pt>1 || n_nm<1) // no more than one point, at least one digit
        return false;
    // skip the trailing whitespaces
    while (s[char_pos] == ' ') {
        ++ char_pos;
    }
    return char_pos == s.size(); // must reach the ending 0 of the string
}

// <居住地> -> 居住地
std::string parseVar(std::string roughVar)
{
    RegularExpression re("<([^<]*)>");
    //std::string var;
    if (re.match(roughVar))
    {
        re.subst(roughVar, "$1");
        return roughVar;
    }
    return "";
}

/// 列目錄內的樣板檔
std::list<std::string> templLists(bool isBasename)
{
    std::set<std::string> files;
    std::list<std::string> rets;
    Poco::Glob::glob("/usr/share/NDCODFAPI/ODFReport/templates/*.ot[ts]", files);

    for (auto it = files.begin() ; it != files.end(); ++ it)
    {
        auto afile = *it;
        auto basename = Poco::Path(afile).getBaseName();
        if (isBasename)
            rets.push_back(basename);
        else
            rets.push_back(Poco::Path(afile).toString());
    }
#ifdef ENABLE_DEBUG
    // 可以在 runTimeData/templates 放範例檔案來針對 API 進行測試
    Poco::Glob::glob("./runTimeData/templates/*.ot[ts]", files);
    for (auto it = files.begin() ; it != files.end(); ++ it)
    {
        auto afile = *it;
        auto basename = Poco::Path(afile).getBaseName();
        if (isBasename)
            rets.push_back(basename);
        else
            rets.push_back(Poco::Path(afile).toString());
    }
#endif
    return rets;
}

/// 將 xml 內容存回 .xml 檔
void saveXmlBack(AutoPtr<Poco::XML::Document> docXML,
        std::string xmlfile)
{
    std::ostringstream ostrXML;
    DOMWriter writer;
    //writer.setNewLine("\n");
    //writer.setOptions(Poco::XML::XMLWriter::PRETTY_PRINT);
    writer.writeNode(ostrXML, docXML);
    const auto xml = ostrXML.str();

    Poco::File f(xmlfile);
    f.setSize(0);  // truncate

    Poco::FileOutputStream fos(xmlfile, std::ios::binary);
    fos << xml;
    fos.close();
    //std::cout << xmlfile << std::endl;
}

/// 以檔名開啟
Parser::Parser(std::string templfile)
    :success(true),
    picserial(0),
    outAnotherJson(false),
    outYaml(false)
{
    extract(templfile);
}

/// 以 rest endpoint 開啟
Parser::Parser(Poco::URI &uri)
    :success(true),
    picserial(0),
    outAnotherJson(false),
    outYaml(false)
{
    //把存在的 .ot[ts] 檔案之路徑生成一個 list
    auto lsts = templLists(false);
    for (auto it = lsts.begin(); it != lsts.end(); ++ it)
    {
        const auto templfile = *it;
        auto endpoint = Poco::Path(templfile).getBaseName();
        if (uri.toString() == (resturl + endpoint) ||
                uri.toString() == (resturl + endpoint + "?outputPDF") ||
                uri.toString() == (resturl + endpoint + "?outputPDF=") ||
                uri.toString() == (resturl + endpoint + "?outputPDF=true") ||
                uri.toString() == (resturl + endpoint + "?outputPDF=false")
           )  // 符合 endpoint
        {
            extract(templfile);
            return;
        }
    }
    success = false;
}

/// deconstructor
Parser::~Parser()
{
    //std::cout << "DeConstructor" << std::endl;
    /// 移除解壓縮目錄
    Poco::File(extra2).remove(true);
    std::cout << "remove: " << extra2 << std::endl;
}

/// set flags for /api /yaml or /json
void Parser::setOutputFlags(bool anotherJson, bool yaml)
{
    outAnotherJson = anotherJson;
    outYaml = yaml;
}

/// 以 Parser(URI) 開啟時，解開該樣板檔成功與否的判斷依據
bool Parser::isValid()
{
    return success;
}

/// is text?
bool Parser::isText()
{
    return doctype == DocType::TEXT;
}

/// is spreadsheet?
bool Parser::isSpreadSheet()
{
    return doctype == DocType::SPREADSHEET;
}

/// mimetype used for http response's header
std::string Parser::getMimeType()
{
    switch (doctype)
    {
        case DocType::TEXT:
        default:
            return "application/vnd.oasis.opendocument.text";
        case DocType::SPREADSHEET:
            return "application/vnd.oasis.opendocument.spreadsheet";
    }
}

/// 將樣板檔解開
void Parser::extract(std::string templfile)
{
    extra2 = TemporaryFile::tempName();

    std::ifstream inp(templfile, std::ios::binary);
    assert (inp.good());
    Decompress dec(inp, extra2);
    dec.decompressAllFiles();
    assert (!dec.mapping().empty());

    zipfilepaths = dec.mapping();
    for (auto it = zipfilepaths.begin(); it != zipfilepaths.end(); ++it)
    {
        const auto fileName = it->second.toString();
        if (fileName == "content.xml")
            contentXmlFileName = extra2 + "/" + fileName;

        if (fileName == "META-INF/manifest.xml")
            metaFileName = extra2 + "/" + fileName;
    }
}

/// 傳回樣板變數的值
std::string Parser::varKeyValue(const std::string line,
        const std::string key)
{
    StringTokenizer tokens(line, ";", tokenOpts);
    for(size_t idx = 0; idx < tokens.count(); idx ++)
    {
        StringTokenizer keyval(tokens[idx], ":", tokenOpts);
        //std::cout << keyval[0] << "::::" << keyval[1] << std::endl;
        if (0 == Poco::icompare(keyval[0], key))
        {
            if (Poco::toLower(key) == "type")
            {
                if (0 == Poco::icompare(keyval[1], "image"))
                    return "file";
                if (0 == Poco::icompare(keyval[1], "enum"))
                    return "enum";
                if (0 == Poco::icompare(keyval[1], "auto"))
                    return "auto";
                if (0 == Poco::icompare(keyval[1], "boolean"))
                    return "boolean";
                if (0 == Poco::icompare(keyval[1], "float"))
                    return "float";
                if (0 == Poco::icompare(keyval[1], "percentage"))
                    return "percentage";
                if (0 == Poco::icompare(keyval[1], "currency"))
                    return "currency";
                if (0 == Poco::icompare(keyval[1], "date"))
                    return "date";
                if (0 == Poco::icompare(keyval[1], "time"))
                    return "time";
                if (0 == Poco::icompare(keyval[1], "Statistic"))
                    return "statistic";
                return "string";
            }
            if (keyval.count() == 2)
                return keyval[1];
            return "";  // split: 切完後若 aa: 後面沒字了
        }
    }
    return "";
}

// Type:Enum;Items:"男,女";Descript:"""
// Type:String;Description:""
// Type:String;Format:民國年/月/日
std::string Parser::parseJsonVar(std::string var,
        std::string vardata,
        bool anotherJson=false,
        bool yaml=false)
{
    std::string typevar = varKeyValue(vardata, "Type");
    std::string enumvar = varKeyValue(vardata, "Items");
    std::string descvar = varKeyValue(vardata, "Description");
    std::string formatvar = varKeyValue(vardata, "Format");
    std::string apihelpvar = varKeyValue(vardata, "ApiHelp");
    std::string databuf;

    if (typevar == "enum" && !enumvar.empty())
    {
        //std::cout << "found Enum: " << enumvar << std::endl;
        if (yaml)
        {
            Poco::replaceInPlace(enumvar, "\"", "");
            StringTokenizer tokens(enumvar, ",", tokenOpts);
            std::string enumvardata = "                \"enum\": [";
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                const auto tok = tokens[idx];
                enumvardata += "\"" + tok + "\"";
                if (idx != tokens.count() - 1)
                    enumvardata += ",";
            }
            enumvardata += "]\n";
            databuf += enumvardata;
        }
        else
        {
            Poco::replaceInPlace(enumvar, "\"", "");
            StringTokenizer tokens(enumvar, ",", tokenOpts);
            std::string enumvardata = ",\n                        \"enum\":[";
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                const auto tok = tokens[idx];
                enumvardata += "\"" + tok + "\"";
                if (idx != tokens.count() - 1)
                    enumvardata += ",";
            }
            enumvardata += "]";
            databuf += enumvardata;
        }
    }
    //apihelpvar = "api說明";
    //descvar = "描述";
    if (!descvar.empty() || !apihelpvar.empty())
    {
        //std::cout << "found desc: " << descvar << std::endl;
        if (yaml)
        {
            Poco::replaceInPlace(descvar, "\"", "");
            databuf += "                \"description\": \"";
            if (!apihelpvar.empty())
                databuf += apihelpvar;
            if (!descvar.empty() && !apihelpvar.empty())
                databuf += "\n";
            if (!descvar.empty())
                databuf += descvar;
            databuf += "\"";
            databuf += "\n";
        }
        else
        {
            Poco::replaceInPlace(descvar, "\"", "");
            Poco::replaceInPlace(descvar, "\n", "<br />");  // @TODO: need?
            databuf += ",\n                        \"description\": \"";
            if (!apihelpvar.empty())
                databuf += apihelpvar;
            if (!descvar.empty() && !apihelpvar.empty())
                databuf += " / ";
            if (!descvar.empty())
                databuf +=  descvar;
            databuf += "\"";
        }
    }
    if (!formatvar.empty())
    {
        //std::cout << "found format: " << formatvar << std::endl;
        if (yaml)
        {
            Poco::replaceInPlace(formatvar, "\"", "");
            databuf += "                \"format\": \"";
            databuf += formatvar + "\"";
            databuf += "\n";
        }
        else
        {
            Poco::replaceInPlace(formatvar, "\"", "");
            databuf += ",\n                        \"format\": \""
                + formatvar + "\"";
        }
    }

    std::string realtype = varKeyValue(vardata, "Type");

    //auto jvalue = !formatvar.empty() ? formatvar : !descvar.empty() ? descvar : "";
    auto jjvalue = realtype;
    jjvalue += "  // ";
    if (!apihelpvar.empty())
        jjvalue += apihelpvar;
    if (!descvar.empty() && !apihelpvar.empty())
        jjvalue += " / ";
    if (!descvar.empty())
        jjvalue += descvar;

    std::string jvalue = "字串";
    if (realtype == "file")
    {
        jvalue = "array";
        //jjvalue = "base64 編碼";
        if (!yaml)
        {
            /*databuf +=
              ",\n                        \"description\": \"上傳圖片說明\",";*/
            databuf += R"MULTILINE(,
                        "items": {
                        "type": "string",
                        "format": "binary"
                      })MULTILINE";
        }
        else
        {
            //databuf += "                \"description\": \"";
            //databuf += "上傳圖片說明\"";
            //databuf += "\n";
            databuf += "                \"items\":";
            databuf += "\n";
            databuf += "                  \"type\": \"";
            databuf += "string\"";
            databuf += "\n";
            databuf += "                  \"format\": \"";
            databuf += "binary\"";
            databuf += "\n";
        }
    }

    if (realtype == "string")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "字串" : apihelpvar;
    }
    if (realtype == "auto")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "字串" : apihelpvar;
        jjvalue = "string or float";
        jjvalue += "  // ";  // @TODO: 與上段重複
        if (!apihelpvar.empty())
            jjvalue += apihelpvar;
        if (!descvar.empty() && !apihelpvar.empty())
            jjvalue += " / ";
        if (!descvar.empty())
            jjvalue += descvar;
    }
    if (realtype == "float")
    {
        jvalue = "number";
        //jjvalue = apihelpvar.empty() ? "123.45" : apihelpvar;
    }
    if (realtype == "enum")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "1" : apihelpvar;
    }
    if (realtype == "boolean")
    {
        jvalue = "boolean";
        //jjvalue = apihelpvar.empty() ? "true 或 false" : apihelpvar;
    }
    if (realtype == "date")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ? "2018-07-25" : apihelpvar;
    }
    if (realtype == "time")
    {
        jvalue = "string";
        //jjvalue = apihelpvar.empty() ?
        //                "PT09H25M00S(為：09:25:00)" : apihelpvar;
    }
    if (realtype == "percentage")
    {
        jvalue = "number";
        //jjvalue = apihelpvar.empty() ? "0.123" : apihelpvar;
    }
    if (realtype == "currency")
    {
        jvalue = "integer";
        //jjvalue = apihelpvar.empty() ? "100000" : apihelpvar;
    }

    if (anotherJson)
        return "\"" + var + "\": " + "\"" + jjvalue + "\"";

    if (outYaml)
        return Poco::format(YAMLPARAMTEMPL, var, jvalue, databuf);
    else
        return Poco::format(PARAMTEMPL, var, jvalue, databuf);
}

/// get doc type
void Parser::detectDocType()
{
    auto path = "//office:body/office:text";
    auto found = static_cast<Node*>(docXML->getNodeByPath(path));
    if (found)
    {
        //std::cout << "found text:" << std::endl;
        doctype = DocType::TEXT;
    }
    path = "//office:body/office:spreadsheet";
    found = static_cast<Node*>(docXML->getNodeByPath(path));
    if (found)
    {
        //std::cout << "found calc:" << std::endl;
        doctype = DocType::SPREADSHEET;
    }
}

/// translate enum and boolean value
std::string Parser::parseEnumValue(std::string type,
        std::string enumvar,
        std::string value)
{
    if (type == "enum" && isNumber(value))
    {
        std::cout<<enumvar<<std::endl;
        Poco::replaceInPlace(enumvar, "\"", "");
        StringTokenizer tokens(enumvar, ",", tokenOpts);
        int enumIdx = std::stoi(value)-1;
        std::cout << enumIdx << std::endl;
        if (enumIdx >= 0 && (unsigned)enumIdx < tokens.count())
        {
            std::cout << "set enum value: " << tokens[enumIdx] << std::endl;
            value = tokens[enumIdx];
        }
    }
    if (type == "boolean")  // True、Yes、1
    {
        std::cout<<enumvar<<std::endl;
        Poco::replaceInPlace(enumvar, "\"", "");
        StringTokenizer tokens(enumvar, ",", tokenOpts);
        int enumIdx = ("1" == value ||
                0 == Poco::icompare(value, "true") ||
                0 == Poco::icompare(value, "yes")) ? 0 : 1;
        //std::cout << enumIdx << std::endl;
        std::cout << "set enum value: " << tokens[enumIdx] << std::endl;
        value = tokens[enumIdx];
    }
    return value;
}

/// meta-inf: xxx-template -> xxx
/// for bug: excel/word 不能開啟 xxx-template 的文件
std::string Parser::replaceMetaMimeType(std::string attr)
{
    Poco::replaceInPlace(attr,
            "application/vnd.oasis.opendocument.text-template",
            "application/vnd.oasis.opendocument.text");
    Poco::replaceInPlace(attr,
            "application/vnd.oasis.opendocument.spreadsheet-template",
            "application/vnd.oasis.opendocument.spreadsheet");
    return attr;
}

/// for bug: excel/word 不能開啟 xxx-template 的文件
void Parser::updateMetaInfo()
{
    /// meta-inf file
    std::cout << "process manifest" << std::endl;
    InputSource inputSrc(metaFileName);
    DOMParser parser;
    auto docXmlMeta = parser.parse(&inputSrc);
    auto listNodesMeta =
        docXmlMeta->getElementsByTagName("manifest:file-entry");

    //std::cout<<"****"<<listNodesMeta->length()<<std::endl;
    for (unsigned long it = 0; it < listNodesMeta->length(); ++it)
    {
        auto elm = static_cast<Element*>(listNodesMeta->item(it));
        if (elm->getAttribute("manifest:full-path") == "/")
        {
            //std::cout<<elm->getAttribute("manifest:media-type")<<std::endl;
            auto attr = elm->getAttribute("manifest:media-type");
            elm->setAttribute("manifest:media-type",
                    replaceMetaMimeType(attr));
        }
    }
    saveXmlBack(docXmlMeta, metaFileName);

    /// mimetype file
    auto mimeFile = extra2 + "/mimetype";
    Poco::FileInputStream istr(mimeFile);
    std::string mime;
    istr >> mime;
    istr.close();

    mime = replaceMetaMimeType(mime);
    Poco::File f(mimeFile);
    f.setSize(0);  // truncate

    Poco::FileOutputStream fos(mimeFile, std::ios::binary);
    fos << mime;
    fos.close();

    std::cout << "end process manifest" << std::endl;
}

/// write picture info to meta file
void Parser::updatePic2MetaXml()
{
    std::cout << "process manifest" << std::endl;
    InputSource inputSrc(metaFileName);
    DOMParser parser;
    //parser.setFeature(XMLReader::FEATURE_NAMESPACE_PREFIXES, false);
    auto docXmlMeta = parser.parse(&inputSrc);
    auto listNodesMeta =
        docXmlMeta->getElementsByTagName("manifest:manifest");
    auto pElm = docXmlMeta->createElement("manifest:file-entry");
    pElm->setAttribute("manifest:full-path",
            "Pictures/" + std::to_string(picserial));
    pElm->setAttribute("manifest:media-type", "");
    static_cast<Element*>(listNodesMeta->item(0))->appendChild(pElm);

    saveXmlBack(docXmlMeta, metaFileName);
    std::cout << "end process manifest" << std::endl;
}

/// zip it
std::string Parser::zipback()
{
    updateMetaInfo();
    saveXmlBack(docXML, contentXmlFileName);

    // zip
    const auto zip2 = extra2 + ".odf";
    std::cout << "zip2: " << zip2 << std::endl;

    std::ofstream out(zip2, std::ios::binary);
    Compress c(out, true);

    c.addRecursive(extra2);
    c.close();
    return zip2;
}

/// get json
std::string Parser::jsonVars()
{
    jsonvars = "";

    auto allVar = scanVarPtr();
    std::list<Element*> singleVar = allVar[0];
    std::list<Element*> groupVar = allVar[1];

    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";

    std::list<std::string> singleList;
    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        auto elm = *it;
        auto varName = elm->innerText();
        if(isText())
            varName = varName.substr(1, varName.size()-2);
        auto checkExist = std::find(singleList.begin(), singleList.end(), varName);
        if(checkExist != singleList.end())
            continue;
        jsonvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property)) + ",";
        singleList.push_back(varName);
    }
    std::list<std::string> groupList;
    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        auto checkGrpExist = std::find(groupList.begin(), groupList.end(), (*it)->getAttribute("grpname"));
        if(checkGrpExist != groupList.end())
            continue;
        groupList.push_back((*it)->getAttribute("grpname"));

        auto rowVar = (*it)->getElementsByTagName(VAR_TAG);
        int childLen = rowVar->length();
        std::string cells = "";
        std::string grpname = (*it)->getAttribute("grpname");
        std::list<std::string> childVarList;
        for (int i=0; i<childLen; i++)
        {
            auto elm = static_cast<Element*>(rowVar->item(i));
            auto varName = elm->innerText();
            if(isText())
                varName = varName.substr(1, varName.size()-2);
            auto checkVarExist = std::find(childVarList.begin(), childVarList.end(), varName);
            if(checkVarExist != childVarList.end())
                continue;
            childVarList.push_back(varName);
            cells += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property));
            if ((i+1)<childLen)
                cells += ",";
        }
        jsonvars += Poco::format(PARAMGROUPTEMPL, grpname, grpname, cells);
    }
    jsonvars = jsonvars.substr(0, jsonvars.length() - 1);
    return jsonvars;
}

// get json for another
std::string Parser::jjsonVars()
{
    jjsonvars = "";

    auto allVar = scanVarPtr();
    std::list<Element*> singleVar = allVar[0];
    std::list<Element*> groupVar = allVar[1];

    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";

    std::list<std::string> singleList;
    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        auto elm = *it;
        auto varName = elm->innerText();
        if(isText())
            varName = varName.substr(1, varName.size()-2);
        auto checkExist = std::find(singleList.begin(), singleList.end(), varName);
        if (checkExist != singleList.end())
            continue;
        singleList.push_back(varName);
        jjsonvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), true) + ",<br />";
    }

    std::list<std::string> groupList;
    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        auto checkGrpExist = std::find(groupList.begin(), groupList.end(), (*it)->getAttribute("grpname"));
        if(checkGrpExist != groupList.end())
            continue;
        groupList.push_back((*it)->getAttribute("grpname"));

        auto rowVar = (*it)->getElementsByTagName(VAR_TAG);
        int childLen = rowVar->length();
        std::string grpname = (*it)->getAttribute("grpname");

        jjsonvars += "&nbsp;&nbsp;&nbsp;&nbsp;\"" + grpname + "\":[<br />";
        jjsonvars += "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;{";

        std::list<std::string> childVarList;
        for (int i=0; i<childLen; i++)
        {
            auto elm = static_cast<Element*>(rowVar->item(i));
            auto varName = elm->innerText();
            if(isText())
                varName = varName.substr(1,varName.size()-2);
            auto checkVarExist = std::find(childVarList.begin(), childVarList.end(), varName);
            if(checkVarExist != childVarList.end())
                continue;
            childVarList.push_back(varName);

            jjsonvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), true);
            if ((i+1) != childLen)
                jjsonvars += ",";
        }
        jjsonvars += "}";
        jjsonvars += "<br />&nbsp;&nbsp;&nbsp;&nbsp;]";



        auto kk = it;
        if ((kk++) != (groupVar.end()))
            jjsonvars += ",";

        jjsonvars += "<br />";
    }
    if (jjsonvars.substr(jjsonvars.length() - 7, 7) == ",<br />")
    {
        jjsonvars = jjsonvars.substr(0, jjsonvars.length() - 7);
        jjsonvars += "<br />";
    }
    return jjsonvars;
}

// get yaml
std::string Parser::yamlVars()
{
    yamlvars = "";

    auto allVar = scanVarPtr();
    std::list<Element*> singleVar = allVar[0];
    std::list<Element*> groupVar = allVar[1];

    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";


    std::list<std::string> singleList;
    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        auto elm = *it;
        auto varName = elm->innerText();
        if(isText())
            varName = varName.substr(1, varName.size()-2);
        auto checkExist = std::find(singleList.begin(), singleList.end(), varName);
        if(checkExist != singleList.end())
            continue;

        singleList.push_back(varName);
        yamlvars += parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), false, true);
    }

    std::list<std::string> groupList;
    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        auto checkGrpExist = std::find(groupList.begin(), groupList.end(), (*it)->getAttribute("grpname"));
        if(checkGrpExist != groupList.end())
            continue;
        groupList.push_back((*it)->getAttribute("grpname"));
        auto rowVar = (*it)->getElementsByTagName(VAR_TAG);
        int childLen = rowVar->length();
        std::string grpname = (*it)->getAttribute("grpname");
        std::string cells = "";

        std::list<std::string> childVarList;
        for (int i=0; i<childLen; i++)
        {
            auto elm = static_cast<Element*>(rowVar->item(i));
            auto varName = elm->innerText();
            if(isText())
                varName = varName.substr(1, varName.size()-2);
            auto checkVarExist = std::find(childVarList.begin(), childVarList.end(), varName);
            if(checkVarExist != childVarList.end())
                continue;
            childVarList.push_back(varName);

            std::string var = parseJsonVar(varName, elm->getAttribute(Var_Tag_Property), outAnotherJson, outYaml);
            std::string newSpaceVar;

            /// 補上空白 = ident 符合 array
            StringTokenizer tokens(var, "\n", StringTokenizer::TOK_IGNORE_EMPTY);
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                newSpaceVar += "      " + tokens[idx] + "\n";
            }
            cells += newSpaceVar;
        }
        yamlvars += Poco::format(YAMLPARAMGROUPTEMPL, grpname, grpname, cells);
    }
    return yamlvars;
}

// 取出單一變數與群組變數的記憶體位置
std::vector<std::list<Element*>> Parser::scanVarPtr()
{
    // Load XML to program
    InputSource inputSrc(contentXmlFileName);
    DOMParser parser;
    parser.setFeature(XMLReader::FEATURE_NAMESPACES, false);
    parser.setFeature(XMLReader::FEATURE_NAMESPACE_PREFIXES, true);
    docXML = parser.parse(&inputSrc);

    AutoPtr<NodeList> listNodes;
    std::list <VarData> listvars;
    std::list <Element*> singleVar;
    std::list <Element*> groupVar;
    std::vector <std::list<Element*>> result;

    detectDocType();

    if (isText())
    {

        // Scan All Var Pointer save into list
        listNodes = docXML->getElementsByTagName("text:placeholder");
        int totalVar = listNodes->length();
        for (int idx=0; idx < totalVar; idx++)
        {
            Element* currentNode = static_cast<Element*>(listNodes->item(idx));
            auto Parent_1 = static_cast<Element*>(currentNode->parentNode());
            auto Parent_2 = Parent_1->parentNode();
            while(true){
                std::string nodeName = Parent_2->nodeName();
                if(nodeName == "office:text" || nodeName == "table:table-cell")
                {
                    break;
                }
                Parent_2 = Parent_2->parentNode();
            }
            auto Parent_3 = static_cast<Element*>(Parent_2->parentNode());
            if(Parent_2->nodeName() != "table:table-cell")
            {
                singleVar.push_back(currentNode);
            }
            else
            {
                auto grpNodeList = Parent_3->getElementsByTagName("office:annotation");
                int grpLen = grpNodeList->length();
                if (grpLen == 0)
                {
                    singleVar.push_back(currentNode);
                }
                else
                {
                    // If there are different office:annotation name, only take the first grpname as target
                    std::string grpname = grpNodeList->item(0)->lastChild()->innerText();
                    Parent_3->setAttribute("grpname", grpname);
                    auto checkDuplicate = std::find(groupVar.begin(), groupVar.end(), Parent_3);
                    if (checkDuplicate == groupVar.end())
                        groupVar.push_back(Parent_3); 
                }

            }
        }

        // 刪掉 grp tag
        auto grpNodeList = docXML->getElementsByTagName("office:annotation");
        int grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
        grpNodeList = docXML->getElementsByTagName("office:annotation-end");
        grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
    }
    if (isSpreadSheet())
    {
        // Scan All Var Pointer save into list
        listNodes = docXML->getElementsByTagName("text:a");
        int totalVar = listNodes->length();
        for (int idx=0; idx < totalVar; idx++)
        {
            Element* currentNode = static_cast<Element*>(listNodes->item(idx));
            std::string vardata  = currentNode->getAttribute("office:target-frame-name");
            std::string type     = varKeyValue(vardata, "type");
            auto Parent_1 = static_cast<Element*>(currentNode->parentNode());
            auto Parent_2 = static_cast<Element*>(Parent_1->parentNode());
            while(true){
                std::string nodeName = Parent_2->nodeName();
                if(nodeName == "table:table" || nodeName == "table:table-row-group")
                {
                    break;
                }
                Parent_2 = static_cast<Element*>(Parent_2->parentNode());
            }
            Parent_2 = static_cast<Element*> (Parent_2);
            // 如果是 SC 的範本精靈把群組去掉後會保留 table-row-group 所以要雙重檢測
            if(Parent_2->nodeName() == "table:table")
            {
                singleVar.push_back(currentNode);
            }
            // 儘管是群組中的統計變數也要拉出來個別處理，不然在 setGroupVar 無法進行全域的 jsonData 掃描
            else if (type == "statistic")
            {
                singleVar.push_back(currentNode);
            }
            else
            {
                auto grpNodeList = Parent_2->getElementsByTagName("office:annotation");
                int grpLen = grpNodeList->length();
                if (grpLen == 0)
                {
                    singleVar.push_back(currentNode);
                }
                else
                {
                    // If there are different office:annotation name, only take the first grpname as target
                    std::string grpname = grpNodeList->item(0)->lastChild()->innerText();
                    //Ensure put attr grpname in the table:table-row not in table:table-row-group!
                    Parent_2 = static_cast<Element*> (Parent_2->firstChild());
                    while(true)
                    {
                        if (Parent_2->nodeName()=="table:table-row")
                            break;
                        Parent_2 = static_cast<Element*> (Parent_2->firstChild());
                    }
                    Parent_2->setAttribute("grpname", grpname);
                    auto checkDuplicate = std::find(groupVar.begin(), groupVar.end(), Parent_2);
                    if (checkDuplicate == groupVar.end())
                        groupVar.push_back(Parent_2); 
                }

            }
        }

        // 刪掉 grp tag
        auto grpNodeList = docXML->getElementsByTagName("office:annotation");
        int grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
        grpNodeList = docXML->getElementsByTagName("office:annotation-end");
        grpLen = grpNodeList->length();
        for(auto tmp_grp_len = 0; tmp_grp_len < grpLen; tmp_grp_len++)
        {
            auto grpNode = grpNodeList->item(0);
            grpNode->parentNode()->removeChild(grpNode);
        }
    }

    result.push_back(singleVar);
    result.push_back(groupVar);
    return result;
}

// Insert value into group Variable
void Parser::setGroupVar(Object::Ptr jsonData, std::list<Element*> &groupVar)
{
    // Text & SC 的變數 xml tag 有所不同
    std::string VAR_TAG;
    if(isText())
        VAR_TAG = "text:placeholder";
    else if(isSpreadSheet())
        VAR_TAG = "text:a";

    for (auto it = groupVar.begin(); it!=groupVar.end(); it++)
    {
        Element* row = *it;
        Node* currentRow = row;
        Node* realBaseRow = currentRow;
        Node *nextRow;
        Node *rootTable;
        Node* pTbRow ;

        // 針對 Array 的存取目前我們只能作到透過 Var 先判定一次資料是否存在，然後在轉成 Array，如果直接針對 Array 取值會導致無法判斷是否為空的 Array
        Array::Ptr arr;
        int lines = 0;
        std::string grpname = row->getAttribute("grpname");
        if (jsonData->has(grpname))
        {
            Var tmpData = jsonData->get(grpname);
            if(tmpData.isArray())
            {
                arr = tmpData.extract<Array::Ptr>();
                lines = arr->size();
            }
            else
            {
                row->parentNode()->removeChild(row);
                continue;
            }
        }
        else
        {
            row->parentNode()->removeChild(row);
            continue;
        }

        /* 初始化「樣板列」的過程 Text & SC 的 xml 結構有所差異
        */

        Node* initRow = nullptr;
        if(isSpreadSheet())
        {
            // 初始化樣板列: 
            // 1.移除非變數的欄位之內含儲存格內容以及儲存格之特性
            // 2.移除統計變數 (只去除第一行以後的)
            initRow = realBaseRow->cloneNode(true);
            auto child = static_cast<Element*>(initRow->firstChild());//table:table-cell
            while(child)
            {
                if(child->getElementsByTagName("text:a")->length()==0)
                {
                    if (child->getElementsByTagName("text:p")->length()!=0)
                    {
                        auto target = static_cast<Element*>(child->firstChild());
                        while(target)
                        {
                            if(target->nodeName()=="text:p")
                            {
                                child->removeChild(target);
                            }

                            target = static_cast<Element*>(target->nextSibling());
                        }

                    }
                    // 清除 table:table-cell 的 attribute
                    child->removeAttribute("office:value");
                    child->removeAttribute("office:value-type");
                    child->removeAttribute("calcext:value-type");
                    child->removeAttribute("table:formula");
                }
                else
                {
                    // 移除統計變數
                    // 前端設計工具限定一個儲存格只有一個變數
                    auto variableList = child->getElementsByTagName("text:a");
                    Element* target = static_cast<Element*> (variableList->item(0));
                    auto vardata =  target->getAttribute("office:target-frame-name");
                    auto type = varKeyValue(vardata, "type");
                    if(type == "statistic")
                    {
                        child->removeChild(target->parentNode());
                        child->removeAttribute("office:value");
                        child->removeAttribute("office:value-type");
                        child->removeAttribute("calcext:value-type");
                    }
                }
                child = static_cast<Element*>(child->nextSibling());
            }
            // 擴增跨列的行數
            Node* targetNode = realBaseRow;
            while(targetNode->nodeName() != "table:table-row-group")
                targetNode = targetNode->parentNode();

            Element* spanRow;
            if(targetNode->previousSibling()!=NULL)
                spanRow = static_cast<Element*> (targetNode->previousSibling()->firstChild());
            else
                spanRow = static_cast<Element*> (targetNode);

            while(spanRow)
            {
                if (spanRow->hasAttribute("table:number-rows-spanned"))
                    spanRow->setAttribute("table:number-rows-spanned", std::to_string(lines+1));
                spanRow = static_cast<Element*> (spanRow->nextSibling());
            }
        }
        else if (isText())
        {
            // 初始化整列: 主要是去除非編號(1.\n 2. ...etc)的欄位之數值
            initRow = realBaseRow->cloneNode(true);
            auto child = static_cast<Element*>(initRow->firstChild());
            while(child)
            {
                if(child->getElementsByTagName(VAR_TAG)->length()==0)
                {
                    if (child->getElementsByTagName("text:list")->length()==0)
                        if(child->childNodes()->length()!=0)
                            child->removeChild(child->getElementsByTagName("text:p")->item(0));
                }

                child = static_cast<Element*>(child->nextSibling());
            }
            // 擴增跨列的行數
            auto spanRow = static_cast<Element*> (realBaseRow->previousSibling()->firstChild());
            while(spanRow)
            {
                if (spanRow->hasAttribute("table:number-rows-spanned"))
                    spanRow->setAttribute("table:number-rows-spanned", std::to_string(lines+1));
                spanRow = static_cast<Element*> (spanRow->nextSibling());
            }
        }

        /// 列群組：add rows, then set form var data
        for (int times = 0; times < lines; times ++)
        {  
            if (times==0)
                //保留第一行的格式不變
                pTbRow = realBaseRow->cloneNode(true);
            else
                pTbRow = initRow->cloneNode(true);
            // insert new row to the table
            nextRow = currentRow->nextSibling();
            rootTable = currentRow->parentNode();
            rootTable->insertBefore(pTbRow, nextRow);
            currentRow = pTbRow;

            /// put var values into group
            auto rowChildVar = (static_cast<Element*>(pTbRow))->getElementsByTagName(VAR_TAG);
            int childLen = rowChildVar->length();
            std::list<Element*> varList;
            for (int i=0; i<childLen; i++)
            {
                varList.push_back(static_cast<Element*> (rowChildVar->item(i)));
            }

            auto arrData = arr->getObject(times);
            if(times==0)
            {
                for(auto each=varList.begin(); each!=varList.end(); each++)
                {
                    std::string eachName = (*each)->innerText();
                    
                    Var value;
                    if (isText())
                        value = jsonData->get(eachName.substr(1, eachName.size()-2));
                    else if(isSpreadSheet())
                        value = jsonData->get(eachName);

                    if (!value.isEmpty())
                    {
                        arrData->set(eachName, value);
                    }
                }
            }
            setSingleVar(arr->getObject(times), varList);
            
        }
        // Remove template Row
        row->parentNode()->removeChild(row);
    }
}

// Insert into single Variable
void Parser::setSingleVar(Object::Ptr jsonData, std::list<Element*> &singleVar)
{
    /* 函數說明
     *  jsonData 的來源有可能是 request or setGroupVar's jsonData' Array 而來
     *  singleVar 跟 jsonData 來源類似
     */

    //初始化 Text & SC 的 Tag 區別
    //  1. 只有 tag 內含的 property 需要區分
    std::string Var_Tag_Property;
    if(isText())
        Var_Tag_Property = "text:description";
    else if(isSpreadSheet())
        Var_Tag_Property = "office:target-frame-name";

    for (auto it = singleVar.begin(); it!=singleVar.end(); it++)
    {
        Element* elm = *it;
        auto vardata = elm->getAttribute(Var_Tag_Property);
        std::string type = varKeyValue(vardata, "type");

        // 模板變數的類型需要針對 file 特別處理，因為 file 需要把檔案寫在 extract 的資料夾內部
        if (type != "file" and type != "statistic") 
        {
            std::string key = elm->innerText();
            Var value;
            if (isText())
                value = jsonData->get(key.substr(1,key.size()-2));
            else if(isSpreadSheet())
                value = jsonData->get(key);

            if (value.isEmpty())
            {
                elm->parentNode()->removeChild(elm);
                continue;
            }

            // 根據 json 拿到的 value 作數值轉換 (boolean, list)
            auto enumvar = varKeyValue(vardata, "Items");
            auto format = varKeyValue(vardata, "Format");
            value = parseEnumValue(type, enumvar, value.toString());


            // 依照不同型別進行個別處理
            if (type == "auto" && isNumber(value) && isSpreadSheet())
            {
                auto meta = static_cast<Element*>(elm->parentNode()->parentNode());
                auto pVal = docXML->createTextNode(value);
                elm->parentNode()->replaceChild(pVal, elm);
                type = "float";
                meta->setAttribute("office:value", value);
                meta->setAttribute("office:value-type", type);
                meta->setAttribute("calcext:value-type", type);
            }
            else if ( (type == "float" || type == "percentage" ||
                    type == "currency" || type == "date" ||
                    type == "time" )
                    && isSpreadSheet())
            {

                auto meta = static_cast<Element*>(elm->parentNode()->parentNode());
                auto pVal = docXML->createTextNode(value);
                elm->parentNode()->replaceChild(pVal, elm);
                meta->setAttribute("office:value-type", type);
                meta->setAttribute("calcext:value-type", type);
                auto officeValue = "office:" + format;
                meta->setAttribute(officeValue, value);
            }
            else {
                // Writer 一定跑到這裡來
                auto pVal = docXML->createTextNode(value);
                elm->parentNode()->replaceChild(pVal, elm);
            }
        }
        else if (type == "statistic")
        {
            std::string grpname = varKeyValue(vardata, "groupname");
            std::string column = varKeyValue(vardata, "column");
            std::string method = varKeyValue(vardata, "method");
            std::string targetVariable = varKeyValue(vardata, "Items");

            StringTokenizer tokens(column, ".", tokenOpts);
            std::string cell = tokens[1];
            StringTokenizer addr(cell, "$", tokenOpts);
            // addr[0] 是 欄位代號 :ex A
            // addr[1] 是 列位編號 :ex 1
            std::string cellAddr = addr[0] + addr[1];
            column = addr[0];

            Array::Ptr arr;
            int lines;
            if (jsonData->has(grpname))
            {
                Var tmpData = jsonData->get(grpname);
                if(tmpData.isArray())
                {
                    arr = tmpData.extract<Array::Ptr>();
                    lines = arr->size();
                }
                else
                {
                    elm->parentNode()->removeChild(elm);
                    continue;
                }
            }
            else
            {
                elm->parentNode()->removeChild(elm);
                continue;
            }
            std::cout << "group size : " << lines << std::endl;
            auto newElm = docXML->createElement("table:table-cell");
            //TODO use method to repalce SUM
            if (method == "總和")
                method = "SUM";
            if (method == "最大值")
                method = "MAX";
            if (method == "最小值")
                method = "MIN";
            if (method == "中位數")
                method = "MEDIAN";
            if (method == "計數")
                method = "COUNT";
            if (method == "平均")
                method = "AVERAGE";
            std::string formula = "of:="+ method +"([."+cellAddr+":."+column+std::to_string(std::stoi(addr[1])+lines-1)+"])";
            newElm->setAttribute("table:formula", formula);
            newElm->setAttribute("office:value-type", "float");
            newElm->setAttribute("calcext:value-type", "float");
            auto pCell = elm->parentNode()->parentNode();
            pCell->parentNode()->replaceChild(newElm, pCell);
        }
        else if (type == "file")
        {
            //Write file into extract directory
            std::string varname = elm->innerText();
            Var value;
            if (isText())
            {
                varname = varname.substr(1, varname.size()-2);
                value = jsonData->get(varname);
            }
            else if(isSpreadSheet())
                value = jsonData->get(varname);

            if (value.isEmpty())
            {
                elm->parentNode()->removeChild(elm);
                continue;
            }

            auto enumvar = varKeyValue(vardata, "Items");
            value = parseEnumValue(type, enumvar, value);


            auto tempPath = Path::forDirectory(TemporaryFile::tempName() + "/");
            File(tempPath).createDirectories();
            const Path filenameParam(varname);
            tempPath.setFileName(filenameParam.getFileName());
            auto _filename = tempPath.toString();

            try
            {
                // Write b64encode data to image
                std::stringstream ss;
                ss << value.toString();
                Poco::Base64Decoder b64in(ss);
                std::ofstream ofs(_filename);
                std::copy(std::istreambuf_iterator<char>(b64in),
                        std::istreambuf_iterator<char>(),
                        std::ostreambuf_iterator<char>(ofs));
            }
            catch (Poco::Exception& e)
            {
                std::cerr << e.displayText() << std::endl;
            }


            // Write file info to Xml tag
            updatePic2MetaXml();

            if (isText())
            {
                auto desc = elm->getAttribute(Var_Tag_Property);

                // image size
                auto imageSize = varKeyValue(desc, "Size");
                std::string width = "2.5cm", height = "1.5cm";
                if (!imageSize.empty())
                {
                    StringTokenizer token(imageSize, "x", tokenOpts);
                    width = token[0] + "cm";
                    height = token[1] + "cm";
                }

                auto pElm = docXML->createElement("draw:frame");
                pElm->setAttribute("draw:style-name", "fr1");
                pElm->setAttribute("draw:name", "Image1");
                pElm->setAttribute("text:anchor-type", "as-char");
                pElm->setAttribute("svg:width", width);
                pElm->setAttribute("svg:height", height);
                pElm->setAttribute("draw:z-index", "1");

                auto pChildElm = docXML->createElement("draw:image");
                pChildElm->setAttribute("xlink:href",
                        "Pictures/" + std::to_string(picserial));
                pChildElm->setAttribute("xlink:type", "simple");
                pChildElm->setAttribute("xlink:show", "embed");
                pChildElm->setAttribute("xlink:actuate", "onLoad");
                pChildElm->setAttribute("loext:mime-type", "image/png");
                pElm->appendChild(pChildElm);

                auto node = elm->parentNode();
                node->replaceChild(pElm, elm);

                const auto picdir = extra2 + "/Pictures";
                Poco::File(picdir).createDirectory();
                const auto picfilepath = picdir + "/" +
                    std::to_string(picserial);
                Poco::File(_filename).copyTo(picfilepath);
                picserial ++;
            }
            else if (isSpreadSheet())
            {
                auto desc = elm->getAttribute(Var_Tag_Property);

                // image size
                auto imageSize = varKeyValue(desc, "Size");
                std::string width = "2.5cm", height = "1.5cm";
                if (!imageSize.empty())
                {
                    StringTokenizer token(imageSize, "x", tokenOpts);
                    width = token[0] + "cm";
                    height = token[1] + "cm";
                }

                auto pElm = docXML->createElement("draw:frame");
                pElm->setAttribute("draw:style-name", "gr1");
                pElm->setAttribute("draw:name", "Image1");
                pElm->setAttribute("svg:width", width);
                pElm->setAttribute("svg:height", height);
                pElm->setAttribute("draw:z-index", "1");

                auto pChildElm = docXML->createElement("draw:image");
                pChildElm->setAttribute("xlink:href", "Pictures/" + std::to_string(picserial));
                pChildElm->setAttribute("xlink:type", "simple");
                pChildElm->setAttribute("xlink:show", "embed");
                pChildElm->setAttribute("xlink:actuate", "onLoad");
                pChildElm->setAttribute("loext:mime-type", "image/png");
                pElm->appendChild(pChildElm);

                // 直接替換掉整個儲存格，避免遺留不必要的特性
                auto newCell = docXML->createElement("table:table-cell");
                auto oldCell = elm->parentNode()->parentNode();
                auto node = elm->parentNode()->parentNode()->parentNode();

                newCell->appendChild(pElm);
                node->replaceChild(newCell, oldCell);

                const auto picdir = extra2 + "/Pictures";
                Poco::File(picdir).createDirectory();
                const auto picfilepath = picdir + "/" +
                    std::to_string(picserial);
                Poco::File(_filename).copyTo(picfilepath);
                picserial ++;
            }
        }
    }
}

/// Handles the filename part of the convert-to POST request payload.
class ConvertToPartHandler2 : public PartHandler
{
    public:
        NameValueCollection vars;  /// post filenames
    private:
        std::string& _filename;  /// current post filename

    public:
        ConvertToPartHandler2(std::string& filename)
            : _filename(filename)
        {
        }

        virtual void handlePart(const MessageHeader& header,
                std::istream& stream) override
        {
            // Extract filename and put it to a temporary directory.
            std::string disp;
            NameValueCollection params;
            if (header.has("Content-Disposition"))
            {
                std::string cd = header.get("Content-Disposition");
                MessageHeader::splitParameters(cd, disp, params);
            }

            if (!params.has("filename"))
                return;
            if (params.get("filename").empty())
                return;

            auto tempPath = Path::forDirectory(
                    TemporaryFile::tempName() + "/");
            File(tempPath).createDirectories();
            // Prevent user inputting anything funny here.
            // A "filename" should always be a filename, not a path
            const Path filenameParam(params.get("filename"));
            tempPath.setFileName(filenameParam.getFileName());
            _filename = tempPath.toString();

            // Copy the stream to _filename.
            std::ofstream fileStream;
            fileStream.open(_filename);
            StreamCopier::copyStream(stream, fileStream);
            fileStream.close();

            vars.add(params.get("name"), _filename);
            fprintf(stderr, "handle part, %s\n", _filename.c_str());
        }
};

MergeODF::MergeODF()
{}

/// init. logger
void MergeODF::setLogPath(std::string)
{
    std::cout<<"mergeodf: setlogpath"<<std::endl;
    logdb = new LogDB();
    logdb->setDbPath();
}

/// api help. yaml&json&json sample(another json)
std::string MergeODF::makeApiJson(std::string which="",
        bool anotherJson,
        bool yaml,
        bool showHead)
{
    std::string jsonstr;

    auto templsts = templLists(false);
    auto it = templsts.begin();
    for (size_t pos = 0; it != templsts.end(); ++it, pos++)
    {
        try
        {
            const auto templfile = *it;
            Parser *parser = new Parser(templfile);
            parser->setOutputFlags(anotherJson, yaml);

            auto endpoint = Poco::Path(templfile).getBaseName();

            if (!which.empty() && endpoint != which)
                continue;


            std::string buf;
            std::string parserResult;
            if (anotherJson)
            {
                buf = "* json 傳遞的 json 資料需以 urlencode(encodeURIComponent) 編碼<br />"
                    "* 圖檔需以 base64 編碼<br />"
                    "* 若以 json 傳參數，則 header 需指定 content-type='application/json'<br /><br />json 範例:<br /><br />";
                buf += Poco::format("{<br />%s}", parser->jjsonVars());
            }
            else if (yaml)
                buf = Poco::format(YAMLTEMPL, endpoint, endpoint, parser->yamlVars());
            else
                buf = Poco::format(APITEMPL, endpoint, endpoint, parser->jsonVars());

            delete parser;

            jsonstr += buf;

            if (!which.empty() && endpoint == which)
                break;

            if (pos != templsts.size()-1 && !yaml)
                jsonstr += ",";
        }
        catch (const std::exception & e)
        {
            //...
        }
    }

    //cout << jsonstr << endl;
    const auto& app = Poco::Util::Application::instance();
    const auto ServerName = app.config().getString("server_name");
    // add header
    if (showHead && !anotherJson)
    {
        std::string read;
        if (yaml)
            read = Poco::format(YAMLTEMPLH, ServerName, jsonstr);
        else
            read = Poco::format(TEMPLH, ServerName, jsonstr);
        return read;
    }

    return jsonstr;
}

/// validate if match rest uri: /accessTime
std::string MergeODF::isMergeToQueryAccessTime(std::string uri)
{
    auto templsts = templLists(true);
    for (auto it = templsts.begin(); it != templsts.end(); ++it)
    {
        const auto endpoint = *it;
        if (uri == (resturl + endpoint + "/accessTime"))
            return endpoint;
    }
    return "";
}

/// validate if match rest uri
std::string MergeODF::isMergeToUri(std::string uri, bool forHelp,
        bool anotherJson, bool yaml)
{
    auto templsts = templLists(true);
    for (auto it = templsts.begin(); it != templsts.end(); ++it)
    {
        const auto endpoint = *it;
        if (forHelp)
        {
            if (uri == (resturl + endpoint + "/api"))
                return endpoint;
            if (uri == (resturl + endpoint + "/yaml") && yaml)
                return endpoint;
            if (uri == (resturl + endpoint + "/json") && anotherJson)
                return endpoint;
        }
        else
        {
            //std::cout<<uri<<std::endl;
            if (uri == (resturl + endpoint) ||
                    uri == (resturl + endpoint + "?outputPDF=false"))
                return endpoint;
            if (uri == (resturl + endpoint + "?outputPDF") ||
                    uri == (resturl + endpoint + "?outputPDF=") ||
                    uri == (resturl + endpoint + "?outputPDF=true"))
                return "pdf";
        }
    }
    return "";
}

/// validate if match rest uri for /mergeto/[doc]/api
/// anotherJson=true for /mergeto/[doc]/json
std::string MergeODF::isMergeToHelpUri(std::string uri,
        bool anotherJson,
        bool yaml)
{
    return isMergeToUri(uri, true, anotherJson, yaml);
}

/// mimetype used for http response's header
std::string MergeODF::getMimeType()
{
    // content-type: only for swagger
    return "application/octet-stream";
}

// document's extname, used for content-disposition
std::string MergeODF::getDocExt()
{
    if (mimetype == "application/vnd.oasis.opendocument.text")
        return "odt";
    if (mimetype == "application/vnd.oasis.opendocument.spreadsheet")
        return "ods";
    return "odt";
}

/// json: 關鍵字轉小寫，但以quote包起來的字串不處理
std::string MergeODF::keyword2Lower(std::string in, std::string keyword)
{
    RegularExpression re(keyword, RegularExpression::RE_CASELESS);
    RegularExpression::Match match;

    auto matchSize = re.match(in, 0, match);

    //std::cout << match.size()<<std::endl;
    while (matchSize > 0)
    {
        // @TODO: add check for "   null   "
        if (in[match.offset - 1] != '"' &&
                in[match.offset + keyword.size()] != '"')
        {
            for(unsigned idx = 0; idx < keyword.size(); idx ++)
                in[match.offset + idx] = keyword[idx];
        }
        matchSize = re.match(in, match.offset + match.length, match);
    }
    return in;
}

/// 解析表單陣列： 詳細資料[0][姓名] => 詳細資料:姓名
Object::Ptr MergeODF::parseArray2Form(HTMLForm &form)
{
    // {"詳細資料": [ {"姓名": ""} ]}
    std::map <std::string,
        std::vector<std::map<std::string, std::string>>
            > grpNames;
    // 詳細資料[0][姓名] => {"詳細資料": [ {"姓名": ""} ]}
    Object::Ptr formJson = new Object();

    for (auto iterator = form.begin();
            iterator != form.end();
            iterator ++)
    {
        const auto varname = iterator->first;
        const auto value = iterator->second;
        //std::cout <<"===>"<< varname << std::endl;

        auto res = "^([^\\]\\[]*)\\[([^\\]\\[]*)\\]\\[([^\\]\\[]*)\\]$";
        RegularExpression re(res);
        RegularExpression::MatchVec posVec;
        re.match(varname, 0, posVec);
        //std::cout<<"reg size:"<<posVec.size()<<std::endl;
        if (posVec.empty())
        {
            formJson->set(varname, Var(value));
            continue;
        }

        const auto grpname = varname.substr(posVec[1].offset,
                posVec[1].length);
        const auto grpidxRaw = varname.substr(posVec[2].offset,
                posVec[2].length);
        const auto grpkey = varname.substr(posVec[3].offset,
                posVec[3].length);
        const int grpidx = std::stoi(grpidxRaw);

        //std::vector<std::map<std::string, std::string>> dummy;
        if (grpNames.find(grpname) == grpNames.end())
        {  // default array
            std::vector<std::map<std::string, std::string>> dummy;
            grpNames[grpname] = dummy;
        }

        // 詳細資料[n][姓名]: n to resize
        // n 有可能 1, 3, 2, 6 不照順序, 這裡以 n 當作 resize 依據
        // 就可以調整陣列大小了
        if (grpNames[grpname].size() < (unsigned)(grpidx + 1))
            grpNames[grpname].resize(grpidx + 1);
        //std::cout<<"grpidx:"<<grpidx<<std::endl;
        grpNames[grpname].at(grpidx)[grpkey] = value;
    }
    // {"詳細資料": [ {"姓名": ""} ]} => 詳細資料:姓名=value
    //
    for(auto itgrp = grpNames.begin();
            itgrp != grpNames.end();
            itgrp++)
    {
        //std::cout<<"***"<<itgrp->first<<std::endl;
        auto gNames = itgrp->second;
        for(unsigned grpidx = 0; grpidx < gNames.size(); grpidx ++)
        {
            auto names = gNames.at(grpidx);
            Object::Ptr tempData = new Object();
            for(auto itname = names.begin();
                    itname != names.end();
                    itname++)
            {
                tempData->set(itname->first, Var(itname->second));
            }
            if (names.size() != 0 )
            {
                if(!formJson->has(itgrp->first))
                {
                    Array::Ptr newArr = new Array();
                    formJson->set(itgrp->first, newArr);
                }
                formJson->getArray(itgrp->first)->add(tempData);
            }
        }
    }
    return formJson;
}

/// get api called times
int MergeODF::getApiCallTimes(std::string endpoint)
{
    logdb->setApi(endpoint);
    return logdb->getAccessTimes();
}


/// merge to odf file
std::string MergeODF::doMergeTo(const Poco::Net::HTTPRequest& request,
        Poco::MemoryInputStream& message)
{
    std::cout << "mergeto--->" << std::endl;
    std::string fromPath;
    auto requestUri = Poco::URI(request.getURI());

    Parser *parser = new Parser(requestUri);
    std::cout << "mergeto--->2" << std::endl;
    if (!parser->isValid())
    {
        mergeStatus = MergeStatus::TEMPLATE_NOT_FOUND;
        return "";
    }

    ConvertToPartHandler2 handler(fromPath);
    Object::Ptr object;

    if (request.getContentType() == "application/json")
    {
        std::string line, data;
        std::istream &iss(message);
        while (!iss.eof())
        {
            std::getline(iss, line);
            data += line;
        }
        // 解析 request body to json
        std::string jstr = data;

        jstr = keyword2Lower(jstr, "null");
        jstr = keyword2Lower(jstr, "true");
        jstr = keyword2Lower(jstr, "false");
        Poco::JSON::Parser jparser;
        Var result;

        // Parse data to PocoJSON 
        try{
            result = jparser.parse(jstr);
            object = result.extract<Object::Ptr>();
        }
        catch (Poco::Exception& e)
        {
            std::cerr << e.displayText() << std::endl;
            mergeStatus = MergeStatus::JSON_PARSE_ERROR;
            return false;
        }
    }
    else
    {
        HTMLForm form;
        form.setFieldLimit(0);
        form.load(request, message, handler);
        // 資料形式如果是 post HTML Form 上來
        try {
            object = parseArray2Form(form);
        }
        catch (Poco::Exception& e)
        {
            std::cerr << e.displayText() << std::endl;
        }
    }

    mimetype = parser->getMimeType();

    // XML 前處理:  遍歷文件的步驟都要在這裡處理,不然隨著文件的內容增加,會導致遍歷時間大量增長

    //把 form 的資料放進 xml 檔案
    auto allVar = parser->scanVarPtr();
    std::list<Element*> singleVar = allVar[0];
    std::list<Element*> groupVar = allVar[1];

    parser->setSingleVar(object, singleVar);
    parser->setGroupVar(object, groupVar);
    const auto zip2 = parser->zipback();
    delete parser;
    return zip2;
}

/// merge status
MergeODF::MergeStatus MergeODF::getMergeStatus(void)
{
    return mergeStatus;
}

/// 轉檔：轉成 odf
std::string MergeODF::outputODF(std::string outfile)
{
    lok::Office *llo = NULL;
    try
    {
        llo = lok::lok_cpp_init(loPath.c_str());
        if (!llo)
        {
            std::cout << ": Failed to initialise LibreOfficeKit" << std::endl;
            return "";
        }
    }
    catch (const std::exception & e)
    {
        delete llo;
        std::cout << ": LibreOfficeKit threw exception (" << e.what() << ")" << std::endl;
        return "";
    }

    char *options = 0;
    lok::Document * lodoc = llo->documentLoad(outfile.c_str(), options);
    if (!lodoc)
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to load document (" << errmsg << ")" << std::endl;
        return "";
    }

    outfile = outfile + ".pdf";
    //std::cout << outfile << std::endl;
    if (!lodoc->saveAs(outfile.c_str(), "pdf", options))
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to export (" << errmsg << ")" << std::endl;

        //Poco::File(outfile).remove(true);
        //std::cout << "remove: " << odffile << std::endl;

        delete lodoc;
        return "";
    }

    //Poco::File(outfile).remove(true);
    //std::cout << "remove: " << odffile << std::endl;
    delete lodoc;

    return outfile;
}

/// response 回傳 api 的呼叫次數
void MergeODF::responseAccessTime(std::weak_ptr<StreamSocket> _socket, std::string endpoint)
{
    int access = getApiCallTimes(endpoint);

    std::ostringstream oss;
    std::string accessTime = "{\"call_time\": " + std::to_string(access) + std::string("}");
    oss << "HTTP/1.1 200 OK\r\n"
        << "Last-Modified: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
        << "Access-Control-Allow-Origin: *" << "\r\n"
        << "User-Agent: " << WOPI_AGENT_STRING << "\r\n"
        << "Content-Length: " << accessTime.size() << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "\r\n"
        << accessTime;

    auto socket = _socket.lock();
    socket->send(oss.str());
    socket->shutdown();
}

/// http://server/lool/merge-to
/// called by LOOLWSD
void MergeODF::handleMergeTo(std::weak_ptr<StreamSocket> _socket,
        const Poco::Net::HTTPRequest& request,
        Poco::MemoryInputStream& message)
{
    HTTPResponse response;
    auto socket = _socket.lock();

    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.set("Access-Control-Allow-Headers",
            "Origin, X-Requested-With, Content-Type, Accept");

    // process convert to pdf
    Process::PID pid = fork();
    if (pid < 0)
    {
        response.setStatusAndReason
            (HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
             "error loading mergeodf");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        _exit(Application::EXIT_SOFTWARE);
        return;
    }
    else if (pid == 0)
    {
        if ((pid = fork()) < 0)
        {
            response.setStatusAndReason(
                    HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                    "error loading mergeodf");
            response.setContentLength(0);
            socket->send(response);
            socket->shutdown();
            _exit(Application::EXIT_SOFTWARE);
            return;
        }
        else if (pid > 0)
        {
            _exit(Application::EXIT_SOFTWARE);
            return;
        }
        else
        {
            std::cout << getpid()<<std::endl;

            std::string zip2;
            auto endpointPath = Poco::URI(request.getURI()).getPath();
            auto endpoint = Poco::Path(endpointPath).getBaseName();
            logdb->setApi(endpoint);
            logdb->notice(_socket, response, "start");

            try{
                logdb->notice(_socket, response, "merging");
                zip2 = doMergeTo(request, message);
            }
            catch (const std::exception & e)
            {
                logdb->notice(_socket, response, "merge error");

                response.setStatusAndReason
                    (HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                     "merge error");
                response.setContentLength(0);
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }
            /*if (getMergeStatus() == MergeStatus::PARAMETER_REQUIRE)
              {
              response.setStatusAndReason(HTTPResponse::HTTP_UNAUTHORIZED,
              "parameter not given");
              socket->send(response);
              socket->shutdown();
              return;
              }*/
            if (getMergeStatus() == MergeStatus::JSON_PARSE_ERROR)
            {
                logdb->notice(_socket, response, "Json data error");

                response.setStatusAndReason(HTTPResponse::HTTP_UNAUTHORIZED,
                        "Json data error");
                response.setContentLength(0);
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }
            logdb->notice(_socket, response, "merging done");

            auto mimeType = getMimeType();

            const auto toPdf = isMergeToUri(request.getURI()) == "pdf";
            auto docExt = !toPdf ? getDocExt() : "pdf";
            response.set("Content-Disposition",
                    "attachment; filename=\"" + endpoint + "."+ docExt +"\"");

            if (!toPdf)
            {
                HttpHelper::sendFile(socket, zip2, mimeType, response);

                Poco::File(zip2).remove(true);
                std::cout << "remove: " << zip2 << std::endl;
                _exit(Application::EXIT_SOFTWARE);
                return;
            }

            logdb->notice(_socket, response, "start convert to pdf");

            auto zip2pdf = outputODF(zip2);
            if (zip2pdf.empty() || !Poco::File(zip2pdf).exists())
            {
                std::cout<<"zip2pdf.epmty()"<<std::endl;

                logdb->notice(_socket, response, "merging to pdf error");

                response.setStatusAndReason
                    (HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                     "merge error");
                response.setContentLength(0);
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }

            logdb->notice(_socket, response, "convert to pdf: done");

            HttpHelper::sendFile(socket, zip2pdf, mimeType, response);
            Poco::File(zip2).remove(true);
            Poco::File(zip2pdf).remove(true);
            std::cout << "remove: " << zip2pdf << std::endl;

            std::cout << "convert-to: shutdown"<<getpid()<<std::endl;
            _exit(Application::EXIT_SOFTWARE);
        }
    }
    else
    {
        std::cout << "call from parent" << std::endl;
        waitpid(pid, NULL, 0); // 父程序呼叫waitpid(), 等待子程序終結,並捕獲返回狀態
    }
}
