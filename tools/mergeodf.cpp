#include "mergeodf.h"

#include <sys/wait.h>
#include <sys/resource.h>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <Poco/RegularExpression.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Net/HTTPResponse.h>
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
#include <Poco/FileChannel.h>
#include <Poco/AsyncChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>

using Poco::Net::HTMLForm;
using Poco::Net::MessageHeader;
using Poco::Net::PartHandler;
using Poco::Net::HTTPResponse;
using Poco::RegularExpression;
using Poco::Zip::Compress;
using Poco::Zip::Decompress;
using Poco::Path;
using Poco::File;
using Poco::FileChannel;
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
using Poco::PatternFormatter;
using Poco::XML::XMLReader;

const std::string resturl = "/lool/merge-to/";
const int tokenOpts = StringTokenizer::TOK_IGNORE_EMPTY |
                      StringTokenizer::TOK_TRIM;

extern "C" MergeODF* create_object()
{
  return new MergeODF;
}

static Poco::Logger& logger()
{
    return Application::instance().logger();
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

// <居住地>
// ->
// 居住地
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
        std::cout << basename << std::endl;
    }
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


std::string GroupVar::prefix(std::string grpname, std::string varname = "")
{
    const auto pref = grpname + std::string(":") + varname;
    return pref;
}

/// remove group row
void GroupVar::remove(Element* anchor)
{
    auto groupStart = static_cast<Node*>(*elms(anchor).begin());
    // <table> remove -> <table-row>
    groupStart->parentNode()  // table-row
              ->parentNode()  // table
              ->removeChild(groupStart->parentNode());
}

/// get group list: all row
std::list<Element*> GroupVar::baserow()
{
    // 新增另一group後，前一group的變數會消失(取代為表單的值)
    // 因此每次列變數都不一樣，這裡只設定第一次
    //if (baseRows.size() > 0)
        //return baseRows;
    std::list<Element*> ret;

    auto lsts = docXML->getElementsByTagName("office:annotation");
    for (unsigned long idx = 0; idx < lsts->length(); ++ idx)
    {
        auto anchor = static_cast<Element*>(lsts->item(idx));
        auto prop = anchor->getElementsByTagName("dc:creator");
        if (prop->length() != 1)
            continue;
        if (static_cast<Element*>(prop->item(0))->innerText() !=
             "OSSII")
            continue;

        prop = anchor->getElementsByTagName("text:p");
        if (prop->length() != 1)
            continue;
        prop = static_cast<Element*>(prop->item(0))->
                    getElementsByTagName("text:span");
        if (prop->length() != 1)
        {  // 沒有 <text:span> 則直接抓裡面文字
            prop = anchor->getElementsByTagName("text:p");
        }
        anchor->setAttribute("grpname",
                              static_cast<Element*>(prop->item(0))->innerText());
        ret.push_back(anchor);
    }
    return ret;
}

/// get group list data
std::list <Node*> GroupVar::elms(Element* anchor)
{
    std::list <Node*> groupList;

    auto anchorName = anchor->getAttribute("office:name");
    /// get beginning block of annonation
    auto groupStart = static_cast<Node*>(anchor->parentNode());
    while (groupStart && groupStart->nodeName() != "table:table-cell")
    {  /// @TODO: check if while(...) to loop over
        groupStart = groupStart->parentNode();
        if (!groupStart)
            return groupList;  // no parent
    }
    //std::cout << "found start anno: " << groupStart->nodeName() << std::endl;

    /// append node until end of annotation
    auto groupEnd = groupStart->nextSibling();
    groupList.push_back(groupStart);
    while (groupEnd)
    {
        anchor = static_cast<Element*>(groupEnd);
        auto endAnchorName = anchor->getAttribute("office:name");
        auto annoEndNodes =
                anchor->getElementsByTagName("office:annotation-end");

        groupList.push_back(groupEnd);

        if (annoEndNodes->length() > 0 && anchorName == endAnchorName)
            break;  // found end of annonation

        groupEnd = groupEnd->nextSibling();
    }
    std::cout << "size:" << groupList.size() << std::endl;
    return groupList;
}

/// get group list data: only varname
std::list <std::string> GroupVar::vars()
{
    // 新增另一group後，前一group的變數會消失(取代為表單的值)
    // 因此每次列變數都不一樣，這裡只設定第一次
    if (varnames.size() > 0)
        return varnames;

    //std::list<std::string> ret;
    int rowIdx = 0;
    auto rows = baserow();
    for (auto itrow = rows.begin();
         itrow != rows.end();
         ++ itrow, rowIdx ++)
    {
        auto row = *itrow;
        auto lsts = elms(row);
        for (auto it = lsts.begin(); it != lsts.end(); ++ it)
        {
            auto cur = static_cast<Element*>(*it);
            auto listNodes = cur->getElementsByTagName(Parser::TAG_VAR);

            for (unsigned long idx = 0;
                 idx < listNodes->length();
                 ++ idx)
            {
                auto elm = static_cast<Element*>(listNodes->item(idx));
                auto var = row->getAttribute("grpname") + ":" +
                           parseVar(elm->innerText());
                //std::cout<<var<<std::endl;
                varnames.push_back(var);
            }
        }
    }
    return varnames;
}

std::string GroupVar::firstVar(std::string grpname)
{
    auto lsts = vars();
    for (auto it = lsts.begin(); it != lsts.end(); ++ it)
    {
        const auto var = *it;
        const auto varp = prefix(grpname);
        //std::cout<<"]]"<<var<<"=====>"<<varp<<std::endl;
        if (var.substr(0, varp.length()) == varp)
            return var;
    }
    return "";
}

/// check if var in group list data
bool GroupVar::inVars(std::string var)
{
    auto lsts = vars();
    for (auto it = lsts.begin(); it != lsts.end(); ++ it)
    {
        std::string varname = *it;
        if (var == varname)
            return true;
    }
    return false;
}

/// get group list: all row
std::list<Element*> GroupVarSC::baserow()
{
    std::list<Element*> ret;

    auto listNodes = docXML->getElementsByTagName("office:annotation");
    if (listNodes->length() == 0)
        return ret;

    for (unsigned long it = 0; it < listNodes->length(); ++it)
    {
        auto anchor = static_cast<Element*>(listNodes->item(it));

        auto prop = anchor->getElementsByTagName("text:p");
        if (prop->length() != 1)
            continue;

	//auto anchor = static_cast<Element*>(listNodes->item(0));
        if (anchor->parentNode()->nodeName() != "table:table-cell")
            continue;

        if (anchor->parentNode()->parentNode()->nodeName() !=
            "table:table-row")
            continue;

        anchor = static_cast<Element*>(anchor->parentNode()->
                                            parentNode()->parentNode());
        if (anchor->nodeName() != "table:table-row-group")
            continue;
        auto elm = static_cast<Element*>(anchor->firstChild());
        elm->setAttribute("grpname",
                          static_cast<Element*>(prop->item(0))->innerText());
        //ret.push_back(static_cast<Element*>(anchor->firstChild()));
        ret.push_back(elm);
    }
    return ret;
}

/// get group list data: only varname
std::list <std::string> GroupVarSC::vars()
{
    // 新增另一group後，前一group的變數會消失(取代為表單的值)
    // 因此每次列變數都不一樣，這裡只設定第一次
    if (varnames.size() > 0)
        return varnames;

    const auto TAG_VARDATA_SC = "office:target-frame-name";
    //std::cout << static_cast<Node*>(baserow())->nodeName()<<std::endl;
    auto lsts = baserow();
    int idx = 0;
    for (auto itrow = lsts.begin();
         itrow != lsts.end();
         ++ itrow, idx ++)
    {
        const auto row = *itrow;
        auto listNodes = row->getElementsByTagName("text:a");
        for (unsigned long it = 0; it < listNodes->length(); ++it)
        {
            auto elm = static_cast<Element*>(listNodes->item(it));
            if (!elm->hasAttribute(TAG_VARDATA_SC))
                continue;

            //auto var = prefix(elm->innerText());
            auto var = row->getAttribute("grpname") + ":" +  elm->innerText();
            //std::cout<<"var:"<<var<<std::endl;
            varnames.push_back(var);  // into vardata
        }
    }
    return varnames;
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

/// 預處理群組列表：將群組變數放進 array
void Parser::intoGroupArray(std::string gname,
                            std::string var,
                            std::string desc)
{
    // group name
    //std::cout<<"groupname:"<<gname<<"var:"<<var<<"desc:"<<desc<<std::endl;
    // create idx:0
    if (jsonGrps.find(gname) == jsonGrps.end())
    {
        std::list <std::string> grp;
        jsonGrps[gname] = grp;
    }

    auto buf = parseJsonVar(var, desc, outAnotherJson, outYaml);
    auto it = std::find(jsonGrps[gname].begin(),
                        jsonGrps[gname].end(),
                        buf);
    if (it == jsonGrps[gname].end())  //  去除重複變數名稱
        jsonGrps[gname].push_back(buf);
    /*else
        std::cout<<"group var duplicate=>"<<buf<<std::endl;*/
}

/// iter for var scanning
std::list<VarData> Parser::scanVarsIter(AutoPtr<NodeList> listNodes,
                                        std::list<VarData> data,
                                        const std::string grpname,
                                        bool noDuplicate)
{
    std::list <VarData> listvars;
    listvars.insert(listvars.end(), data.begin(), data.end());

    for (unsigned long it = 0; it < listNodes->length(); ++it)
    {
        auto elm = static_cast<Element*>(listNodes->item(it));
        auto var = parseVar(elm->innerText());
        auto varP = groupvar->prefix(grpname, var);
        auto desc = elm->getAttribute(TAG_VARDATA);
        auto type = varKeyValue(desc, "Type");

        if (noDuplicate && groupvar->inVars(varP))
            continue;

        if (!groupvar->inVars(varP) && isVarsDuplicate(listvars, var))
            continue;  // 去除重複變數名稱
        if (groupvar->inVars(varP))
            intoGroupArray(grpname, var, desc);
        else
        {
            // into json
            jsonvars += parseJsonVar(var, desc) + ",";
            jjsonvars += parseJsonVar(var, desc, true) + ",<br />";
            yamlvars += parseJsonVar(var, desc, false, true);
            //std::cout << var << "\t" << desc << "\t" << type << std::endl;
        }

        listvars.push_back(VarData(var, type));  // into vardata
    }
    return listvars;
}

/// spreadsheet: iter for var scanning
std::list<VarData> Parser::scanVarsIterSC(AutoPtr<NodeList> listNodes,
                                          std::list<VarData> data,
                                          const std::string grpname,
                                          bool noDuplicate)
{
    std::list <VarData> listvars;
    listvars.insert(listvars.end(), data.begin(), data.end());

    // @TODO: 怪？設定這個變數值為 static 以後，lool stop 就會 double free error!（see .h）
    const auto TAG_VARDATA_SC = "office:target-frame-name";
    for (unsigned long it = 0; it < listNodes->length(); ++it)
    {
        auto elm = static_cast<Element*>(listNodes->item(it));
        if (!elm->hasAttribute(TAG_VARDATA_SC))
            continue;

        auto var = elm->innerText();
        auto varP = groupvarsc->prefix(grpname, var);
        auto desc = elm->getAttribute(TAG_VARDATA_SC);
        auto type = varKeyValue(desc, "Type");

        if (noDuplicate && groupvarsc->inVars(varP))
            continue;

        if (!groupvarsc->inVars(varP) && isVarsDuplicate(listvars, var))
            continue;  // 去除重複變數名稱
        if (groupvarsc->inVars(varP))
            intoGroupArray(grpname, var, desc);
        else
        {
            // into json
            jsonvars += parseJsonVar(var, desc) + ",";
            jjsonvars += parseJsonVar(var, desc, true) + ",<br />";
            yamlvars += parseJsonVar(var, desc, false, true);
            //std::cout << var << "\t" << desc << "\t" << type << std::endl;
        }

        listvars.push_back(VarData(var, type));  // into vardata
    }
    return listvars;
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

/// generate jsonvars for group vars
void Parser::parseJsonGrpVars()
{
    for(auto itgrp = jsonGrps.begin();
        itgrp != jsonGrps.end();
        itgrp++)
    {
        auto grpname = itgrp->first;
        auto grpvars = itgrp->second;
        std::string cells = "";

        //std::cout<<grpname<<std::endl;
        for (auto it = grpvars.begin(); it != grpvars.end();)
        {
            std::string var = *it;
            //std::cout<<var<<std::endl;
            cells += var;
            if (++it != grpvars.end())
                cells += ",";
        }

        jsonvars += Poco::format(PARAMGROUPTEMPL, grpname,
                                 grpname, cells);
    }
}

/// generate jsonvars for group vars
void Parser::parseYamlGrpVars()
{
    for(auto itgrp = jsonGrps.begin();
        itgrp != jsonGrps.end();
        itgrp++)
    {
        auto grpname = itgrp->first;
        auto grpvars = itgrp->second;
        std::string cells = "";

        //std::cout<<grpname<<std::endl;
        for (auto it = grpvars.begin(); it != grpvars.end(); ++it)
        {
            std::string var = *it;
            std::string newSpaceVar;

            /// 補上空白 = ident 符合 array
            StringTokenizer tokens(var, "\n",
                StringTokenizer::TOK_IGNORE_EMPTY);
            for(size_t idx = 0; idx < tokens.count(); idx ++)
            {
                //std::cout<<"VAR="<<tokens[idx]<<std::endl;
                newSpaceVar += "      " + tokens[idx] + "\n";
            }
            //std::cout<<"VAR="<<newSpaceVar<<std::endl;
            cells += newSpaceVar;
        }

        yamlvars += Poco::format(YAMLPARAMGROUPTEMPL, grpname,
                                 grpname, cells);
    }
}

/// generate jjsonvars for group vars
void Parser::parseJJsonGrpVars()
{
    for(auto itgrp = jsonGrps.begin(); itgrp != jsonGrps.end();)
    {
        auto grpname = itgrp->first;
        auto grpvars = itgrp->second;

        //std::cout<<"parseJJsonGrpVars()"<<grpname<<std::endl;
        jjsonvars +=
            "&nbsp;&nbsp;&nbsp;&nbsp;\"" + grpname + "\":[<br />";
        jjsonvars +=
            "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;{";
        for (auto it = grpvars.begin(); it != grpvars.end();)
        {
            //std::cout<<*it<<std::endl;
            jjsonvars += *it;
            if (++it != grpvars.end())
                jjsonvars += ",";
        }
        jjsonvars += "}";
        jjsonvars += "<br />&nbsp;&nbsp;&nbsp;&nbsp;]";

        if (++itgrp != jsonGrps.end())
            jjsonvars += ",";

        jjsonvars += "<br />";
    }
    //std::cout<<jjsonvars<<std::endl;
}

/// 檢查是否變數名稱重複？
/// 只限非群組變數
bool Parser::isVarsDuplicate(std::list<VarData> vars, std::string var)
{
    int founds = 0;
    for(auto it = vars.begin(); it != vars.end(); it++)
    {
        const auto roughVar = *it;
        auto varname = roughVar.get<0>();
        if (var == varname)
            founds ++;
    }
    //if (founds>0)
    //    std::cout<<"duplicate=>"<<var<<std::endl;
    return founds > 0;
}

/// 分析樣板變數的值，並列到 json 變數
/// <text:placeholder text:placeholder-type="text" text:description="Type:String"><居住地></text:placeholder>
std::list<VarData> Parser::scanVars()
{
    std::list <VarData> listvars;
    InputSource inputSrc(contentXmlFileName);
    DOMParser parser;
    parser.setFeature(XMLReader::FEATURE_NAMESPACES, false);
    parser.setFeature(XMLReader::FEATURE_NAMESPACE_PREFIXES, true);
    docXML = parser.parse(&inputSrc);
    AutoPtr<NodeList> listNodes;

    detectDocType();

    if (isText())
    {
        // group
        groupvar = new GroupVar(docXML);

        int idx = 0;
        auto rows = groupvar->baserow();
        for (auto itrow = rows.begin();
             itrow != rows.end();
             ++ itrow, idx ++)
        {
            //std::cout << "process row" << std::endl;
            auto row = *itrow;
            const auto grpname = row->getAttribute("grpname");
            auto lsts = groupvar->elms(row);
            for (auto it = lsts.begin(); it != lsts.end(); ++ it)
            {
                auto elm = static_cast<Element*>(*it);
                listNodes = elm->getElementsByTagName(TAG_VAR);
                listvars = scanVarsIter(listNodes, listvars, grpname, false);
            }
        }

        // other vars
        listNodes = docXML->getElementsByTagName(TAG_VAR);
        listvars = scanVarsIter(listNodes, listvars, "", true);
    }
    if (isSpreadSheet())
    {
        groupvarsc = new GroupVarSC(docXML);

        auto lsts = groupvarsc->baserow();
        for (auto it = lsts.begin(); it != lsts.end(); ++ it)
        {
            auto row = static_cast<Element*>(*it);
            const auto grpname = row->getAttribute("grpname");
            listNodes = row->getElementsByTagName("text:a");
            listvars = scanVarsIterSC(listNodes, listvars, grpname, false);
        }

        // other vars
        listNodes = docXML->getElementsByTagName("text:a");
        listvars = scanVarsIterSC(listNodes, listvars, "", true);
    }

    parseJsonGrpVars();
    parseJJsonGrpVars();
    parseYamlGrpVars();

    // 移除最後一個逗點
    jsonvars = jsonvars.substr(0, jsonvars.length() - 1);
    if (jjsonvars.substr(jjsonvars.length() - 7, 7) == ",<br />")
    {
        jjsonvars = jjsonvars.substr(0, jjsonvars.length() - 7);
        jjsonvars += "<br />";
    }
    return listvars;
}

/// 清除沒置換的樣板變數
/// spreadsheet: 移除有問題的 table:number-rows-repeated
void Parser::cleanUnused()
{
    //std::cout << "clean unused vars: " << std::endl;
    AutoPtr<NodeList> listNodes;
    if (isText())
    {
        listNodes = docXML->getElementsByTagName(TAG_VAR);
        while(listNodes->length() > 0)
        {
            auto elm = static_cast<Element*>(listNodes->item(0));
            auto node = elm->parentNode();
            node->removeChild(elm);
            listNodes = docXML->getElementsByTagName(TAG_VAR);
        }
    }
    if (isSpreadSheet())
    {
        listNodes = docXML->getElementsByTagName("text:a");
        while(listNodes->length() > 0)
        {
            auto elm = static_cast<Element*>(listNodes->item(0));
            if (!elm->hasAttribute("office:target-frame-name"))
                continue;

            auto node = elm->parentNode();
            node->removeChild(elm);
            listNodes = docXML->getElementsByTagName("text:a");
        }
        /// 以 excel 另存, 有時會夾帶 table:number-rows-repeated=xxxx
        /// 若此值太大會使 excel 無法開啟, 在此移除該屬性即可開啟
        const auto TAG_ROWS_REPEAT = "table:number-rows-repeated";
        listNodes = docXML->getElementsByTagName("table:table");
        for (unsigned long it = 0; it < listNodes->length(); ++it)
        {  /// 找到最後一個 table_rows_repeat 且值大於五萬
            unsigned long hasRowsRepeatedCount = 0;
            auto elm = static_cast<Element*>(listNodes->item(it));
            auto rowNodes = elm->getElementsByTagName("table:table-row");
            for(unsigned long idx = 0; idx < rowNodes->length(); ++ idx)
            {
                auto row = static_cast<Element*>(rowNodes->item(idx));
                if (!row->hasAttribute(TAG_ROWS_REPEAT))
                    continue;
                hasRowsRepeatedCount ++;
            }
            for(unsigned long idx = 0, curRowsRepeat = 1;
                idx < rowNodes->length();
                ++ idx)
            {
                auto row = static_cast<Element*>(rowNodes->item(idx));
                if (!row->hasAttribute(TAG_ROWS_REPEAT))
                    continue;
                if ((curRowsRepeat == hasRowsRepeatedCount) &&
                     std::stoi(row->getAttribute(TAG_ROWS_REPEAT))
                        > 50000)
                {
                    //std::cout<<"remove num-rows-repeat"<<std::endl;
                    row->removeAttribute(TAG_ROWS_REPEAT);
                    break;
                }
                curRowsRepeat ++;
            }
            std::cout<<"hasRowsRepeatedCount"<<hasRowsRepeatedCount<<std::endl;
        }
    }
}

/// 取得表單複數變數第 pos 個的值
std::string Parser::getFormGroupVarValue(const HTMLForm &form,
                                         std::string var, int pos)
{
    std::string value;
    NameValueCollection::ConstIterator iterator = form.begin();
    for (int idx = 0; iterator != form.end(); iterator ++)
    {
        const auto varname = iterator->first;
        value = iterator->second;
        if (varname == var)
        {
            if (idx == pos)
                return iterator->second;
            idx ++;
        }
    }
    return "";
}

void Parser::appendGroupRow(const HTMLForm &form, Element *baserow,
                              int lines)
{
    auto grpname = baserow->getAttribute("grpname");
    auto lsts = groupvar->elms(baserow);
    Node *groupStart = *(lsts.begin());

    /// 列群組：add rows, then set form var data
    auto it = lsts.begin();
    for (unsigned times = 0; times < (unsigned)lines; times ++)
    {
        auto pTbRow = docXML->createElement("table:table-row");

        for (it = lsts.begin(); it != lsts.end(); ++ it)
        {  // 建立空的列
            auto anode = *it;
            try
            {
                pTbRow->appendChild(anode->cloneNode(true));
            }
            catch (Poco::Exception& e)
            {
                std::cerr << e.displayText() << std::endl;
            }
        }
        groupStart->parentNode()  // <table:table-row>
                  ->parentNode()  // <table:table>
                  ->appendChild(pTbRow);

        /// put var values into group
        auto searchlist = static_cast<Element*>(pTbRow);

        auto varlists = groupvar->vars();
        //auto itvar = varlists.begin();
        for (auto itvar = varlists.begin(); itvar != varlists.end(); ++ itvar)
        {
            auto var = *itvar;
            auto value = getFormGroupVarValue(form, var, times);
            set(grpname, var, value, searchlist->getElementsByTagName(TAG_VAR));
            std::cout << "form set group var: " << var << "value: " << value << std::endl;
        }
    }

    groupvar->remove(baserow);
    //std::cout << "end of parse anno.... " << std::endl;
}

/// 置換樣板變數: group vars
void Parser::set(const HTMLForm &form)
{
    if (isSpreadSheet())
    {
        setSC(form);
        return;
    }
    /// 列群組的變數: 依第一個變數來列 form 的列表數量
    std::cout << "group vars:" << std::endl;
    if (groupvar->vars().size() == 0)
        return;

    unsigned curRowIdx = 0;
    auto lsts = groupvar->baserow();
    for (auto it = lsts.begin(); it != lsts.end(); curRowIdx ++, ++ it)
    {
        const auto row = *it;
        auto grpname = row->getAttribute("grpname");
        auto firstVar = groupvar->firstVar(grpname);

        /// 計算第一個 group var 與表單同名變數共有幾個 = group 有幾列
        unsigned lines = 0;
        auto iterator = form.begin();
        while (iterator != form.end())
        {
            const auto varname = iterator->first;
            if (varname == firstVar)
                lines ++;
            iterator++;
        }
        //std::cout << "group lines to process(list): " << lines << std::endl;
        appendGroupRow(form, row, lines);
    }
}

void Parser::appendGroupRowSC(const HTMLForm &form, Element *baserow,
                              int lines)
{
    const auto path = "//office:annotation";
    auto grpname = baserow->getAttribute("grpname");
    for (int times = lines - 1; times >= 0; times --)
    {
        auto newrow = static_cast<Element*>(baserow)->cloneNode(true);
        auto anchor = static_cast<Node*>(newrow->getNodeByPath(path));
        if(anchor)
            anchor->parentNode()->removeChild(anchor);

        baserow->parentNode()->  // <table:table-row-group>
                 parentNode()->  // <table:table>
                 insertBefore(newrow,
                    baserow->parentNode()->nextSibling());

        auto varlists = groupvarsc->vars();
        for (auto it = varlists.begin(); it != varlists.end(); ++ it)
        {
            auto var = *it;
            auto value = getFormGroupVarValue(form, var, times);

            //std::cout << "var=" << var << "value=" << value << std::endl;
            setSC(grpname, var, value,
                           static_cast<Element*>(newrow)->
                                getElementsByTagName("text:a"));
            //std::cout << "form set group var: " << var << "value: " << value << std::endl;
        }
    }

    baserow->parentNode()->removeChild(baserow);
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
    if (type == "boolean" && isText())  // True、Yes、1
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
/// 置換樣板變數: group vars
void Parser::setSC(const HTMLForm &form)
{
    /// 列群組的變數: 依第一個變數來列 form 的列表數量
    //std::cout << "group vars:" << std::endl;
    if (groupvarsc->vars().size() == 0)
        return;

    unsigned curRowIdx = 0;
    auto lsts = groupvarsc->baserow();
    for (auto it = lsts.begin(); it != lsts.end(); curRowIdx ++, ++ it)
    {
        const auto row = *it;
        auto grpname = row->getAttribute("grpname");
        auto firstVar = groupvarsc->firstVar(grpname);
        std::cout<<"setSC: grpname="<<grpname<<std::endl;

        /// 計算第一個 group var 與表單同名變數共有幾個 = group 有幾列
        unsigned lines = 0;
        auto iterator = form.begin();
        while (iterator != form.end())
        {
            const auto varname = iterator->first;
            if (varname == firstVar)
                lines ++;
            iterator++;
        }
        //std::cout << curRowIdx << "group lines to process(list): " << lines << std::endl;

        appendGroupRowSC(form, row, lines);
    }
}

/// 置換樣板變數: 非圖片
/// groupNodes 有指定：指定群組變數
void Parser::setSC(std::string grpname,
                 std::string varname,
                 std::string value,
                 AutoPtr < NodeList > groupNodes = 0)
{
    auto listNodes = docXML->getElementsByTagName("text:a");
    if (groupNodes)
        listNodes = groupNodes;

    // @TODO: 怪？設定這個變數值為 static 以後，lool stop 就會 double free error!（see .h）
    const auto TAG_VARDATA_SC = "office:target-frame-name";
    for (unsigned long it = 0; it < listNodes->length(); ++ it)
    {
        auto elm = static_cast<Element*>(listNodes->item(it));
        auto var = elm->innerText();
        auto desc = elm->getAttribute(TAG_VARDATA_SC);
        auto type = varKeyValue(desc, "Type");
        std::string enumvar = varKeyValue(desc, "Items");
        auto format = varKeyValue(desc, "Format");

        value = parseEnumValue(type, enumvar, value);

        if (groupNodes)
        {
            var = groupvarsc->prefix(grpname, elm->innerText());
            //std::cout << var << "::::"<<varname<<std::endl;
        }
        if (var != varname)
            continue;

        if (type == "auto" && isNumber(value))
        {
            // 設定儲存格資料型態
//<table:table-cell calcext:value-type="float" office:value="2.2" office:value-type="float">
//    <text:p>2.2</text:p>
//</table:table-cell>

            type = "float";
            auto meta = static_cast<Element*>(
                elm->parentNode()->parentNode());
            auto metap = static_cast<Element*>(elm->parentNode());

            if (metap->childNodes()->length() > 1)
            {  // 數字儲存格：裡面若有他元素就直接置換，不必換成數字
                auto pVal = docXML->createTextNode(value);
                elm->parentNode()->replaceChild(pVal, elm);
            }
            else
            {
                meta->setAttribute("office:value-type", type);
                meta->setAttribute("calcext:value-type", type);
                meta->setAttribute("office:value", value);
            }
        }
        else if (type == "float" || type == "percentage" ||
                 type == "currency" || type == "date" ||
                 type == "time" || type == "boolean")
        {
//<...office:value-type="date" office:date-value="2018-07-26" calcext:value-type="date">
            auto meta = static_cast<Element*>(
                elm->parentNode()->parentNode());
            meta->setAttribute("office:value-type", type);
            meta->setAttribute("calcext:value-type", type);
            auto officeValue = "office:" + format;
            meta->setAttribute(officeValue, value);
        }
        else
        {
            auto pVal = docXML->createTextNode(value);
            elm->parentNode()->replaceChild(pVal, elm);
        }
    }
}

/// 置換樣板變數: 非圖片
/// groupNodes 有指定：指定群組變數
void Parser::set(std::string grpname,
                 std::string varname,
                 std::string value,
                 AutoPtr < NodeList > groupNodes = 0)
{
    if (isSpreadSheet())
    {
        setSC(grpname, varname, value, groupNodes);
        return;
    }
    std::cout << "set value: " << value << std::endl;
    auto listNodes = docXML->getElementsByTagName(TAG_VAR);
    if (groupNodes)
        listNodes = groupNodes;
    for (unsigned long it = 0; it < listNodes->length(); ++ it)
    {
        auto elm = static_cast<Element*>(listNodes->item(it));
        auto var = parseVar(elm->innerText());
        auto vardata = elm->getAttribute(TAG_VARDATA);
        auto enumvar = varKeyValue(vardata, "Items");
        auto type = varKeyValue(vardata, "Type");

        value = parseEnumValue(type, enumvar, value);

        if (groupNodes)
        {
            var = groupvar->prefix(grpname, var);
            //std::cout << var << "::::"<<varname<<std::endl;
        }
        if (var != varname)
            continue;

        // 換行與否取決於是否有 \n
        if (value.find("\n") == std::string::npos && !groupNodes)
        {  // 只有一行則直接取代，不必跟換行一樣切成 array, 否則會造成多一行
           // @TODO: 怪的是，群組變數卻不受影響？
            auto pVal = docXML->createTextNode(value);
            elm->parentNode()->replaceChild(pVal, elm);
        }
        else
        {
            auto node = elm->parentNode();  // <text:p>...</text:p>
            StringTokenizer vals(value, "\n", tokenOpts);
            for(size_t idx = 0; idx < vals.count(); idx ++)
            {
                //std::cout << "parse newline: " << vals[idx] << std::endl;
                auto nodeP = node->cloneNode(false);  // <text:p>
                auto pVal = docXML->createTextNode(vals[idx]);
                nodeP->appendChild(pVal);
                node->parentNode()->insertBefore(nodeP, node);
            }
            node->parentNode()->removeChild(node);
        }
    }
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

/// 置換樣板變數: 圖片
void Parser::set(std::string varname, NameValueCollection uploadvars)
{
    auto listNodes = docXML->getElementsByTagName(TAG_VAR);
    for (unsigned long it = 0; it < listNodes->length(); ++ it)
    {
        auto elm = static_cast<Element*>(listNodes->item(it));
        auto var = parseVar(elm->innerText());

        if (var != varname)
            continue;

        updatePic2MetaXml();

        auto node = elm->parentNode();
        node->removeChild(elm);

        auto desc = elm->getAttribute(TAG_VARDATA);
        //std::cout << "size:" << varKeyValue(desc, "Size") << std::endl;

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

        node->appendChild(pElm);

        const auto picdir = extra2 + "/Pictures";
        Poco::File(picdir).createDirectory();
        const auto picfilepath = picdir + "/" +
                                        std::to_string(picserial);
        Poco::File(uploadvars.get(varname)).copyTo(picfilepath);
        picserial ++;
    }
}

/// zip it
std::string Parser::zipback()
{
    /// 1) clear unused vars
    cleanUnused();

    /// @TODO: no need?
    /// 2) rewrite info. poco xml 會重設 info, 這裡再寫回來.
/*    auto listNodes =
                docXML->getElementsByTagName("office:document-content");
    auto elm = static_cast<Element*>(listNodes->item(0));
    elm->setAttribute("xmlns:style",
                "urn:oasis:names:tc:opendocument:xmlns:style:1.0");
    elm->setAttribute("xmlns:text",
                "urn:oasis:names:tc:opendocument:xmlns:text:1.0");
    elm->setAttribute("xmlns:table",
                "urn:oasis:names:tc:opendocument:xmlns:table:1.0");
    elm->setAttribute("xmlns:draw",
                "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
    elm->setAttribute("xmlns:fo",
        "urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0");
    elm->setAttribute("xmlns:xlink",
                "http://www.w3.org/1999/xlink");
    elm->setAttribute("xmlns:dc",
                "http://purl.org/dc/elements/1.1/");
    elm->setAttribute("xmlns:meta",
                "urn:oasis:names:tc:opendocument:xmlns:meta:1.0");
    elm->setAttribute("xmlns:number",
                "urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0");
    elm->setAttribute("xmlns:svg",
            "urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");
    elm->setAttribute("xmlns:chart",
                "urn:oasis:names:tc:opendocument:xmlns:chart:1.0");
    elm->setAttribute("xmlns:dr3d",
                "urn:oasis:names:tc:opendocument:xmlns:dr3d:1.0");
    elm->setAttribute("xmlns:math",
                "http://www.w3.org/1998/Math/MathML");
    elm->setAttribute("xmlns:of", "urn:oasis:names:tc:opendocument:xmlns:of:1.2");
    elm->setAttribute("xmlns:oooc", "http://openoffice.org/2004/calc");
    elm->setAttribute("xmlns:calcext",
                "urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0");
*/
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
    scanVars();
    return jsonvars;
}
/// get json for another
std::string Parser::jjsonVars()
{
    scanVars();
    return jjsonvars;
}
/// get yaml
std::string Parser::yamlVars()
{
    scanVars();
    return yamlvars;
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
/// 設定 log 檔路徑後直接 init.
void MergeODF::setLogPath(std::string logPath)
{
    std::cout<<"setlogpath"<<std::endl;
    AutoPtr<FileChannel> fileChannel(new FileChannel);

    // 以 AsyncChannel 接 filechannel, 就不會 stop oxool 時 double free error
    // @TODO: 怪異的寫法？要注意若用 poco 其他版本會不會失效
    AutoPtr<Poco::AsyncChannel> pAsync(new Poco::AsyncChannel(fileChannel));

    logPath = "/var/log";
    fileChannel->setProperty("path", logPath + "/mergeodf.log");
    fileChannel->setProperty("archive", "timestamp");
    AutoPtr<PatternFormatter> patternFormatter(new PatternFormatter());
    patternFormatter->setProperty("pattern","%Y %m %d %L%H:%M:%S: %t");
    channel = new Poco::FormattingChannel(patternFormatter, fileChannel);
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
            //std::cout << parser->jsonVars() << std::endl;

            auto endpoint = Poco::Path(templfile).getBaseName();

            if (!which.empty() && endpoint != which)
                continue;

            std::string buf;
            if (anotherJson)
            {
                buf = "* json 傳遞的 json 資料需以 urlencode(encodeURIComponent) 編碼<br />"
"* 圖檔需以 base64 編碼<br />"
"* 若以 json 傳參數，則 header 需指定 content-type='application/json'<br /><br />json 範例:<br /><br />";
                buf += Poco::format("{<br />%s}", parser->jjsonVars());
            }
            else if (yaml)
                buf = Poco::format(YAMLTEMPL, endpoint, parser->yamlVars());
            else
                buf = Poco::format(APITEMPL, endpoint, parser->jsonVars());

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

/// parse json vars to form
bool MergeODF::parseJson(HTMLForm &form)
{
    if (form.empty())
        return true;

    std::string jstr = form.get("parm");

    try{

        jstr = keyword2Lower(jstr, "null");
        jstr = keyword2Lower(jstr, "true");
        jstr = keyword2Lower(jstr, "false");
        //std::cout << jstr << std::endl;

        Poco::JSON::Parser jparser;
        Var result = jparser.parse(jstr);
        Object::Ptr object = result.extract<Object::Ptr>();
        DynamicStruct collec = *(result.extract<Object::Ptr>());

        for (auto it = collec.begin(); it != collec.end(); ++it)
        {
            std::cout << it->first << std::endl;
            //fprintf(stderr, "item : %s\t%d\n", it->second.toString().c_str(), it->second.isArray());
            //fprintf(stderr, "item : %d\n", *object->isNull(it->first));
            //fprintf(stderr, "item : %s\t%d\n", it->second.toString().c_str(), it->second.isNull());
            if (it->second.isArray())
            {
                Array subArr = *object->getArray(it->first);
                for (unsigned idx = 0; idx < subArr.size(); idx ++)
                {
                    std::vector<std::string> grpnames;
                    subArr.getObject(idx)->getNames(grpnames);
                    for (auto grpname : grpnames)
                    {
                        auto grpvalue = subArr.getObject(idx)->
                                            get(grpname);
                        // check for null
                        if (subArr.getObject(idx)->isNull(grpname))
                            grpvalue = "";

                        //std::cout<<grpname<<"==>"<<grpvalue.toString()<<std::endl;
                        auto key = it->first + ":" + grpname;
                        auto value = grpvalue.toString();
                        form.add(key, value);
                    }
                }
            }
            else
            {
                // check for null
                if (object->isNull(it->first))
                    form.set(it->first, "");
                else
                    form.set(it->first, it->second);
            }
        }
    }
    catch (Poco::Exception& e)
    {
        std::cerr << e.displayText() << std::endl;
        mergeStatus = MergeStatus::JSON_PARSE_ERROR;
        return false;
    }
    return true;
}

/// 解析表單陣列： 詳細資料[0][姓名] => 詳細資料:姓名
void MergeODF::parseArray2Form(HTMLForm &form)
{
    // {"詳細資料": [ {"姓名": ""} ]}
    std::map <std::string,
                std::vector<std::map<std::string, std::string>>
                > grpNames;
    // 詳細資料[0][姓名] => {"詳細資料": [ {"姓名": ""} ]}
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
            continue;

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
    for(auto itgrp = grpNames.begin();
        itgrp != grpNames.end();
        itgrp++)
    {
        //std::cout<<"***"<<itgrp->first<<std::endl;
        auto gNames = itgrp->second;
        for(unsigned grpidx = 0; grpidx < gNames.size(); grpidx ++)
        {
            auto names = gNames.at(grpidx);
            for(auto itname = names.begin();
                itname != names.end();
                itname++)
            {
                //std::cout<<"("<<grpidx<<")"<<"*****"<<itname->first;
                //std::cout<<":"<<itname->second<<std::endl;
                const auto formfield = itgrp->first + ":" + itname->first;
                form.add(formfield, itname->second);
            }
        }
    }
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

    auto lsts = parser->scanVars();
    auto iter = lsts.begin();

    //std::cout << "mergeto--->" << form.begin()->first << std::endl;
    ConvertToPartHandler2 handler(fromPath);
    HTMLForm form;
    form.setFieldLimit(0);

    if (request.getContentType() == "application/json")
    {
        std::istream &iss(message);
        std::string line, data;
        while (!iss.eof())
        {
            std::getline(iss, line);
            data += line;
        }
        form.load(request, message, handler);
        form.add("parm", data);

        if (!parseJson(form))
        {
            mergeStatus = MergeStatus::JSON_PARSE_ERROR;
            return "";
        }
    }
    else
        form.load(request, message, handler);

    try {
        parseArray2Form(form);
    }
    catch (Poco::Exception& e)
    {
        std::cerr << e.displayText() << std::endl;
    }

    mimetype = parser->getMimeType();
    parser->set(form);  // first set group vars...

    // set form vars
    for ( ; iter != lsts.end(); ++iter)
    {
        const auto roughVar = *iter;
        auto varname = roughVar.get<0>();
        const auto type = roughVar.get<1>();

        // set form vars
        if (form.has(varname) && type != "file" )
        {
            //std::cout << "form var:" << varname << std::endl;
            if (!form.get(varname).empty())
                parser->set("", varname, form.get(varname));
        }
        if (request.getContentType() == "application/json")
        {
            // set picture file
            if (form.has(varname) && type == "file")
            {
                auto tempPath = Path::forDirectory(
                                        TemporaryFile::tempName() + "/");
                File(tempPath).createDirectories();
                const Path filenameParam(varname);
                tempPath.setFileName(filenameParam.getFileName());
                auto _filename = tempPath.toString();

                try
                {
                    std::stringstream ss;
                    ss << form.get(varname);
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

                NameValueCollection vars;  /// post filenames
                vars.add(varname, _filename);

                std::cout << "process image:" << _filename << std::endl;
                parser->set(varname, vars);
            }
        }
        else
        {
            // set picture file
            if (handler.vars.has(varname) && type == "file" )
            {
                std::cout << "process image" << std::endl;
                parser->set(varname, handler.vars);
            }
        }
    }

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

/// http://server/lool/merge-to
/// called by LOOLWSD
void MergeODF::handleMergeTo(std::weak_ptr<StreamSocket> _socket,
                             const Poco::Net::HTTPRequest& request,
                             Poco::MemoryInputStream& message)
{
    Application::instance().logger().setChannel(channel);
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
            auto endpoint = Poco::Path(request.getURI()).getBaseName();
            logger().notice(endpoint + ": start process");

            try{
                logger().notice(endpoint + ": start merge");
                zip2 = doMergeTo(request, message);
            }
            catch (const std::exception & e)
            {
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
                response.setStatusAndReason(HTTPResponse::HTTP_UNAUTHORIZED,
                    "Json data error");
                response.setContentLength(0);
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }
            logger().notice(endpoint + ": merge ok");

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

            logger().notice(endpoint + ": start convert to pdf");
            auto zip2pdf = outputODF(zip2);
            if (zip2pdf.empty() || !Poco::File(zip2pdf).exists())
            {
                std::cout<<"zip2pdf.epmty()"<<std::endl;
                response.setStatusAndReason
                    (HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                    "merge error");
                response.setContentLength(0);
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }

            logger().notice(endpoint + ": convert to pdf ok");
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
