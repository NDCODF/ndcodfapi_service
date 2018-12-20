#include "convertto.h"
#include <memory>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <Poco/FileStream.h>
#include <Poco/RegularExpression.h>
#include <Poco/Zip/Compress.h>
#include <Poco/Zip/Decompress.h>
#include <Poco/TemporaryFile.h>
#include <Poco/StringTokenizer.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/Document.h>
#include <Poco/SAX/InputSource.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/Text.h>
#include <Poco/StreamCopier.h>

#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/NameValueCollection.h>
#include <Poco/Util/Application.h>
#include <Poco/FileChannel.h>
#include <Poco/AsyncChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>
#include <Poco/Net/NetworkInterface.h>
#include <Poco/Net/IPAddress.h>
#include <Poco/Net/DNS.h>
#include <Poco/Path.h>
#include <Poco/UUIDGenerator.h>

#include <Poco/Data/Session.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/SQLite/Connector.h>

using Poco::Net::HTMLForm;
using Poco::TemporaryFile;
using Poco::Zip::Compress;
using Poco::Zip::Decompress;
using Poco::Net::HTTPResponse;
using Poco::StringTokenizer;
using Poco::XML::DOMParser;
using Poco::XML::DOMWriter;
using Poco::XML::InputSource;
using Poco::XML::NodeList;
using Poco::File;
using Poco::FileChannel;
using Poco::Path;
using Poco::StreamCopier;
using Poco::Util::Application;
using Poco::PatternFormatter;
using Poco::Net::PartHandler;
using Poco::Net::MessageHeader;
using Poco::Net::NameValueCollection;
using namespace Poco::Data::Keywords;
using Poco::Data::Statement;
using Poco::Data::RecordSet;
using Poco::Data::Session;

extern "C" ConvertTo* create_object()
{
  return new ConvertTo;
}

extern "C" ConvDB* create_object_convdb()
{
  return new ConvDB;
}

static Poco::Logger& logger()
{
    return Application::instance().logger();
}

/// Handles the filename part of the convert-to POST request payload.
class CConvertToPartHandler : public PartHandler
{
    std::string& _filename;
public:
    CConvertToPartHandler(std::string& filename)
        : _filename(filename)
    {
    }

    virtual void handlePart(const MessageHeader& header, std::istream& stream) override
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

        Path tempPath = Path::forDirectory(
                            Poco::TemporaryFile::tempName() + "/");
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
fprintf(stderr, "handle part5, %s\n", params.get("filename").c_str());

    }
};


void WatermarkParser::updateXMLNS(AutoPtr<Poco::XML::Document> docXML)
{
    /// rewrite info. poco xml 會重設 info, 這裡再寫回來.
    /// @TODO duplicate with mergeodf
    AutoPtr<NodeList> listNodes =
                docXML->getElementsByTagName("office:document-styles");
    Element *elm = static_cast<Element*>(listNodes->item(0));
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
}

/// 將 xml 內容存回 .xml 檔
/// @TODO: duplicate with mergeodf.cpp
void WatermarkParser::saveXmlBack(AutoPtr<Poco::XML::Document> docXML,
                 std::string xmlfile)
{
    std::ostringstream ostrXML;
    DOMWriter writer;
    writer.writeNode(ostrXML, docXML);
    std::string xml = ostrXML.str();

    /// 以字串替換將無法 parse 的片段補回去...
    std::list<REPLACEDTAGS>::iterator it = replacetags.begin();
    for ( ; it != replacetags.end(); ++ it)
    {
        const REPLACEDTAGS roughVar = *it;
        std::string from = roughVar.get<0>();
        std::string to = roughVar.get<1>();
        Poco::replaceInPlace(xml, from, to);
    }

    Poco::File f(xmlfile);
    f.setSize(0);  // truncate

    Poco::FileOutputStream fos(xmlfile, std::ios::binary);
    fos << xml;
    fos.close();
    std::cout << xmlfile << std::endl;
}

ConvertTo::ConvertTo()
{}

/// init. logger
/// 設定 log 檔路徑後直接 init.
void ConvertTo::setLogPath(std::string logPath)
{
    AutoPtr<FileChannel> fileChannel(new FileChannel);

    // 以 AsyncChannel 接 filechannel, 就不會 stop oxool 時 double free error
    // @TODO: 怪異的寫法？要注意若用 poco 其他版本會不會失效
    AutoPtr<Poco::AsyncChannel> pAsync(new Poco::AsyncChannel(fileChannel));

    fileChannel->setProperty("path", logPath + "/convert.log");
    fileChannel->setProperty("archive", "timestamp");
    AutoPtr<PatternFormatter> patternFormatter(new PatternFormatter());
    patternFormatter->setProperty("pattern","%Y %m %d %L%H:%M:%S: %t");
    channel = new Poco::FormattingChannel(patternFormatter, fileChannel);
}

/// 設定浮水印參數
void ConvertTo::setWatermarkParams(
               std::string watermarkTitle,
               std::string watermarkAngle,
               std::string watermarkColor,
               std::string watermarkOpacity)
{
    waparser = WatermarkParser(xmlPath);
    waTitle = watermarkTitle;
    waAngle = watermarkAngle;
    waColor = watermarkColor;
    waOpacity = watermarkOpacity;
}

/// help for api json
/// http://server/lool/convert/api
/// http://server/lool/convert/yaml
std::string ConvertTo::makeApiJson(bool yaml, bool showHead)
{
    const auto& app = Poco::Util::Application::instance();
    const auto ServerName = app.config().getString("server_name");

    if (showHead)
    {
        std::string read;
        if (yaml)
            read = Poco::format(YAMLTEMPLH, ServerName, YAMLTEMPL);
        else
            read = Poco::format(TEMPLH, ServerName, TEMPL);
        return read;
    }
    return yaml ? YAMLTEMPL : TEMPL;
}

/// validate for rest uri
bool ConvertTo::validateUri(HTTPResponse& response, std::string uri)
{
    StringTokenizer tokens(uri, "/?");
    // validate /lool/convert-to/[odt|ods|odp|pdf]/...
    if (tokens[3] != "odt" && tokens[3] != "ods" &&
        tokens[3] != "odp" && tokens[3] != "pdf")
    {
        std::cout << "not odt, ods, odp, pdf" << std::endl;
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                                    "not odt, ods, odp, pdf");
        return false;
    }
    // validate /lool/convert-to/odt/[fileurl|filecontent]
    if (tokens[4] != "fileurl" && tokens[4] != "filecontent")
    {
        std::cout << "not fileurl or filecontent" << std::endl;
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                                    "not fileurl or filecontent");
        return false;
    }
    return true;
}

/// check if uri valid this ap.
bool ConvertTo::isConvertTo(std::string uri)
{
    StringTokenizer tokens(uri, "/?");
    return (tokens.count() >= 5 && tokens[2] == "convert-to");
}

/// @TODO: DUPLICATE with Util.cpp
bool ConvertTo::isLocalhost(const std::string& targetHost)
{
    std::string targetAddress;
    try
    {
        targetAddress = Poco::Net::DNS::resolveOne(targetHost).toString();
    }
    catch (const Poco::Exception& exc)
    {
        //Log::warn("Poco::Net::DNS::resolveOne(\"" + targetHost + "\") failed: " + exc.displayText());
        try
        {
            targetAddress = Poco::Net::IPAddress(targetHost).toString();
        }
        catch (const Poco::Exception& exc1)
        {
            //Log::warn("Poco::Net::IPAddress(\"" + targetHost + "\") failed: " + exc1.displayText());
        }
    }

    Poco::Net::NetworkInterface::NetworkInterfaceList list = Poco::Net::NetworkInterface::list(true,true);
    for (auto& netif : list)
    {
        std::string address = netif.address().toString();
        address = address.substr(0, address.find('%', 0));
        if (address == targetAddress)
        {
            //LOG_INF("WOPI host is on the same host as the WOPI client: \"" <<
                    //targetAddress << "\". Connection is allowed.");
            return true;
        }
    }

    //LOG_INF("WOPI host is not on the same host as the WOPI client: \"" <<
            //targetAddress << "\". Connection is not allowed.");
    return false;
}

/// 取得範本內的浮水印 xml
Node* WatermarkParser::getTextWatermarkXML(std::string name)
{
    auto path = "///style:master-page[@style:name='" + name + "']";
    path += "/style:header/text:p";
    path += "/draw:custom-shape[@draw:name='PowerPlusWaterMarkObject']";
    Node *node = static_cast<Node*>(styleXMLBase->getNodeByPath(path));
    return node;
}

/// 產生內建標記 xml
/// 給置換樣板變數回原本xml 用
Element* WatermarkParser::createOurDummyTag(int curReplaceTag)
{
    auto dummy = styleXML->createElement("text:p");
    dummy->setAttribute("draw:style-name",
                    "fr" + std::to_string(curReplaceTag) + "_ossii");
    return dummy;
}

/// <style:header> created if not found
Element* WatermarkParser::parseStyleHeader(Element* root)
{
    const auto headTag = "style:header";
    if (root->getElementsByTagName(headTag)->length() == 0)
        root->appendChild(styleXML->createElement(headTag));

    auto *head = static_cast<Element*>(
                    root->getElementsByTagName(headTag)->item(0));
    return head;
}

/// <text:p> created if not found
Node* WatermarkParser::parseTextP(Element* head)
{
    const auto pTag = "text:p";
    auto *foundPNode = static_cast<Node*>(head->getNodeByPath(pTag));
    std::string pNodeSName = "Header";
    Node *pNode;
    if (!foundPNode)
    {
        //std::cout << "not found: <text:p text:style-name='Header'>" << std::endl;
        Element *elmP = styleXML->createElement(pTag);
        elmP->setAttribute("text:style-name", pNodeSName);
        head->appendChild(elmP);

        const auto path = "text:p[@text:style-name='" + pNodeSName + "']";
        pNode = static_cast<Node*>(head->getNodeByPath(path));
    }
    else
    {
        auto *pNodeElm = static_cast<Element*>(foundPNode);
        pNodeSName = pNodeElm->getAttribute("text:style-name");
        pNode = foundPNode;
    }
    //std::cout << "text:p style:name=" << pNodeSName << std::endl;
    return pNode;
}

/// 產生浮水印xml
bool WatermarkParser::parseDrawCustomShape(
    std::string rootName,
    Node* pNode,
    int curReplaceTag,
    std::string waTitle,
    std::string waAngle)
{
    const auto pTag = "text:p";
    auto *foundWaterNode = static_cast<Node*>(pNode->getNodeByPath("draw:custom-shape[@draw:name='PowerPlusWaterMarkObject']"));

    auto *water = getTextWatermarkXML(rootName);
    if (!water)
        return false;

    try
    {
        auto dummy_p = createOurDummyTag(curReplaceTag);
        if (foundWaterNode)
            pNode->replaceChild(dummy_p, foundWaterNode);
        else
            pNode->appendChild(dummy_p);

        if (!waTitle.empty())
        {
            auto node = static_cast<Element*>(water)->
                            getElementsByTagName(pTag)->item(0);
            node->replaceChild(styleXMLBase->createTextNode(waTitle),
                               node->firstChild());
        }
        if (!waAngle.empty())
        {
            auto *el = static_cast<Element*>(water);
            auto atr = el->getAttribute("draw:transform");

            Poco::RegularExpression re("rotate *\\(([^\\( ]*)\\).*");
            if (re.match(atr))
            {
                auto angle = atr;
                re.subst(angle, "$1");
                auto realAngle = 3.1415926 / 180 *
                                    ::atof(waAngle.c_str());
                Poco::replaceInPlace(atr, angle,
                                     std::to_string(realAngle));
                //std::cout << atr << ":" << angle << std::endl;

                el->setAttribute("draw:transform", atr);
            }
        }
        setReplacedTags(dummy_p, water);
    }
    catch (Poco::Exception& e)
    {
        std::cerr << e.displayText() << std::endl;
        return false;
    }
    return true;
}

/// process <style:style style:family="graphic" style:name="Mgr1">
bool WatermarkParser::parseMgrXml(
        int curReplaceTag,
        const std::string waColor,
        const std::string waOpacity)
{
    Node *styleNode = static_cast<Node*>(
                styleXML->getNodeByPath("///office:automatic-styles"));
    if (!styleNode)
        return false;  // err: 無此 tag 無法繼續

    const auto path = "///style:style[@style:name='Mgr1']";
    auto *mgr1T = static_cast<Node*>(styleXMLBase->getNodeByPath(path));
    auto *foundMgr1 = static_cast<Node*>(styleXML->getNodeByPath(path));

    if (!mgr1T)
        return false;

    auto text_p = createOurDummyTag(curReplaceTag);

    if (foundMgr1)
        foundMgr1->parentNode()->replaceChild(text_p, foundMgr1);
    else
        styleNode->appendChild(text_p);

    auto* elm = static_cast<Element*>(static_cast<Element*>(mgr1T)->
            getElementsByTagName("style:graphic-properties")->item(0));
    if (!waColor.empty())
        elm->setAttribute("draw:fill-color", waColor);
    if (!waOpacity.empty())
    {
        int opacity = 100 - std::stoi(waOpacity);
        const std::string newOpacity = std::to_string(opacity) + "%";
        elm->setAttribute("draw:opacity", newOpacity);
    }
    setReplacedTags(text_p, mgr1T);
    return true;
}

/// 樣板變數置換回真正的樣板xml
void WatermarkParser::setReplacedTags(
            AutoPtr<Element> fake,
            Node* templ)
{
    DOMWriter writer;
    std::ostringstream templXML, fakeXML;

    writer.writeNode(templXML, templ);
    writer.writeNode(fakeXML, fake);
    replacetags.push_back(REPLACEDTAGS(fakeXML.str(), templXML.str()));
}

WatermarkParser::WatermarkParser(){}

WatermarkParser::WatermarkParser(std::string xmlPath)
:progPath(xmlPath)
{}

/// 產生浮水印xml
bool WatermarkParser::insertWaterMark(
        const std::string odffile,
        const std::string waTitle,
        const std::string waAngle,
        const std::string waColor,
        const std::string waOpacity)
{
    if (odffile.empty())
        return false;

    auto extra2 = TemporaryFile::tempName();
    auto tempfile = odffile;

    /// extract
    std::ifstream inp(tempfile, std::ios::binary);
    assert (inp.good());
    Decompress dec(inp, extra2);
    dec.decompressAllFiles();
    assert (!dec.mapping().empty());

    /// base styles xml, 用來撈 <PowerPlusWaterMarkObject>
    std::string baseXmlFile = "/data/build/tmp/oflool_5422/styles.xml";
    if (!File(baseXmlFile).exists())
        baseXmlFile = progPath + "/loleaflet/dist/styles.xml";

    InputSource inputSrcBase(baseXmlFile);
    DOMParser parserBase;
    styleXMLBase = parserBase.parse(&inputSrcBase);

    /// process...
    auto xmlFile = extra2 + "/styles.xml";
    InputSource inputSrc(xmlFile);
    DOMParser parser;
    styleXML = parser.parse(&inputSrc);

    auto dCurReplaceTag = 1;
    if (!parseMgrXml(dCurReplaceTag, waColor, waOpacity))
        return false;

    auto listNodes = styleXML->getElementsByTagName("style:master-page");
    for (unsigned long it = 0; it < listNodes->length(); ++ it)
    {
        auto *root = static_cast<Element*>(listNodes->item(it));
        const auto rootName = root->getAttribute("style:name");

        /// <style:header> created if not found
        auto head = parseStyleHeader(root);
        auto pNode = parseTextP(head);
        parseDrawCustomShape(rootName, pNode, ++dCurReplaceTag,
                             waTitle, waAngle);
    }

    updateXMLNS(styleXML);
    //saveXmlBack(styleXML, "/tmp/style.xml");
    saveXmlBack(styleXML, extra2 + "/styles.xml");

    const auto zip2 = odffile;// + ".odf";
    std::cout << "zip2: " << zip2 << std::endl;

    std::ofstream out(zip2, std::ios::binary);
    Compress c(out, true);

    c.addRecursive(extra2);
    c.close();

    /// 移除解壓縮目錄
    Poco::File(extra2).remove(true);
    std::cout << "remove: " << extra2 << std::endl;
    return true;
}

/// 產生 log
void ConvertTo::log(std::string outfile,
                    bool success,
                    std::chrono::steady_clock::time_point startStamp)
{
    const auto timeSinceStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - startStamp).count();

    std::string msg;

    if (success)
    {
        msg = "轉檔檔名：" + Poco::Path(outfile).getFileName() + "。轉檔結果：成功。轉檔時間:" + std::to_string(timeSinceStartMs * 0.001) + "秒";
    }
    else
    {
        msg = "轉檔結果：失敗。轉檔時間:" + std::to_string(timeSinceStartMs * 0.001) + "秒";
    }
    logger().notice(msg);
    workMessage = msg;
}

/*bool ConvertTo::ttt(const Poco::Net::HTTPRequest& request)
{
    std::string uname;
    std::string passwd;
    for (const auto& it : request)
    {
        if (it.first == "Authorization")
        {
            Poco::Net::HTTPBasicCredentials creds(request);
            uname = creds.getUsername();
            passwd = creds.getPassword();
            std::cout<<creds.getUsername()<<"->"<<creds.getPassword()<<std::endl;
        }
    }
    if (uname == "wind" && passwd == "1234")
        return true;
    return false;
}*/

std::string ConvertTo::getMimeType()
{
    switch (mimetype)
    {
    case LOK_DOCTYPE_TEXT:
    default:
        return "application/vnd.oasis.opendocument.text";
    case LOK_DOCTYPE_SPREADSHEET:
        return "application/vnd.oasis.opendocument.spreadsheet";
    }
}

/// 取得 log 訊息
std::string ConvertTo::getMessage()
{
    return workMessage;
}

/// 轉檔：轉成 odf
std::string ConvertTo::outputODF(const std::string odffile,
                                 const std::string convert2,
                                 const std::string format)
{
    const auto startStamp = std::chrono::steady_clock::now();

    if (odffile.empty())
    {
        log("", false, startStamp);
        return "";
    }

    std::string outfile;
    lok::Office *llo = NULL;
    try
    {
        llo = lok::lok_cpp_init(loPath.c_str());
        if (!llo)
        {
            std::cout << ": Failed to initialise LibreOfficeKit" << std::endl;
            log(outfile, false, startStamp);
            return "";
        }
    }
    catch (const std::exception & e)
    {
        delete llo;
        std::cout << ": LibreOfficeKit threw exception (" << e.what() << ")" << std::endl;
        log(outfile, false, startStamp);
        return "";
    }

    char *options = 0;
    lok::Document * lodoc = llo->documentLoad(odffile.c_str(), options);
    if (!lodoc)
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to load document (" << errmsg << ")" << std::endl;
        log(outfile, false, startStamp);
        return "";
    }

    outfile = convert2.empty() ? odffile + ".odf" : convert2;
    //std::cout << outfile << std::endl;
    if (!lodoc->saveAs(outfile.c_str(), format.c_str(), options))
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to export (" << errmsg << ")" << std::endl;

        //Poco::File(outfile).remove(true);
        //std::cout << "remove: " << odffile << std::endl;

        delete lodoc;
        log(outfile, false, startStamp);
        return "";
    }
    lodoc = llo->documentLoad(outfile.c_str(), options);
    if (!lodoc)
        mimetype = LOK_DOCTYPE_TEXT;
    else
        mimetype = lodoc->getDocumentType();

    //Poco::File(outfile).remove(true);
    //std::cout << "remove: " << odffile << std::endl;
    delete lodoc;
    if (!waTitle.empty())
    {
        bool success = waparser.insertWaterMark(outfile, waTitle,
                                                waAngle, waColor,
                                                waOpacity);
        if (!success)
        {
            log(outfile, false, startStamp);
            return "";
        }
    }

    log(outfile, true, startStamp);
    return outfile;
}

ConvDB::ConvDB() {
    Poco::Data::SQLite::Connector::registerConnector();
}
ConvDB::~ConvDB() {
    Poco::Data::SQLite::Connector::unregisterConnector();
}

/// @TODO: function name need to rename
/// 從設定檔取得資料庫檔案位置名稱 & timeout
void ConvDB::setDbPath()
{
    const auto& app = Poco::Util::Application::instance();

    dbfile = app.config().getString("convert.db_path", "");
    keyTimeout = app.config().getInt("convert.key_timeout", 60);
    std::cout<<"db: "<<dbfile<<std::endl;
    std::cout<<"key time: "<<keyTimeout<<std::endl;
}

/// found the key?
bool ConvDB::hasKey(const std::string key)
{
    return !getKeyExpire(key).empty();
}

/// key expired?
bool ConvDB::hasKeyExpired(const std::string key)
{
    std::string t0 = getKeyExpire(key);
    if (t0.empty())
        return true;

    Poco::Timestamp now;
    std::time_t t1 = now.epochTime();
    //std::cout<<"t1:"<<t1<<std::endl;

    return (t1 - std::atoi(t0.c_str())) > keyTimeout;
}

/// set filename for convert
void ConvDB::setFile(std::string key, std::string file)
{
    Session session("SQLite", dbfile);
    Statement insert(session);
    insert << "UPDATE keylist SET file=? WHERE uuid=?",
                    use(file), use(key), now;
    session.close();
}

/// get convert filename
std::string ConvDB::getFile(std::string key)
{
    if (hasKeyExpired(key))
    {
        std::cout<<"expire"<<std::endl;
        return "";
    }
    Session session("SQLite", dbfile);

    Statement select(session);
    std::string file;
    select << "SELECT file FROM keylist WHERE uuid=?",
                    into(file), use(key);
    while (!select.done())
    {
        select.execute();
        break;
    }
    session.close();
    return file;
}

/// get expires for key
std::string ConvDB::getKeyExpire(std::string key)
{
    Session session("SQLite", dbfile);

    Statement select(session);
    std::string expires;
    select << "SELECT expires FROM keylist WHERE uuid=?",
                    into(expires), use(key);
    while (!select.done())
    {
        select.execute();
        break;
    }
    session.close();
    return expires;
}

/// @TODO: duplicate uuid?
std::string ConvDB::newkey()
{
    //std::cout<<"newkey()"<<std::endl;
    Poco::UUIDGenerator& gen = Poco::UUIDGenerator::defaultGenerator();
    auto key = gen.create().toString();

    Session session("SQLite", dbfile);
    Statement insert(session);
    insert << "INSERT INTO keylist (uuid, expires) VALUES (?, strftime('%s', 'now'))",
              use(key), now;
    session.close();

    return "\"" + key + "\"";
}

/*
 * 驗證 Mac
 * mac_addr=aaa
 * mac_addr=aaa,bbb,ccc
 */
bool ConvDB::validateMac(std::string macStr)
{
    const int tokenOpts = StringTokenizer::TOK_IGNORE_EMPTY |
                          StringTokenizer::TOK_TRIM;

    Session session("SQLite", dbfile);

    Statement select(session);
    StringTokenizer tokens(macStr, ",", tokenOpts);
    for(size_t idx = 0; idx < tokens.count(); idx ++)
    {
        int count;
        auto mac = tokens[idx];
        std::cout<<"mac:"<<mac<<std::endl;
        select << "SELECT COUNT(*) FROM maciplist \
                    WHERE macip=? AND ftype='mac'",
                        into(count), use(mac);
        try
        {
            while (!select.done())
            {
                select.execute();
                break;
            }
        }
        catch (Poco::Exception& e)
        {
            std::cerr << e.displayText() << std::endl;
            session.close();
            return false;
        }
        if (count > 0)
            return true;
    }
    session.close();
    return false;
}

/*
 * 驗證 IP
 */
bool ConvDB::validateIP(std::string clientAddress)
{
    Session session("SQLite", dbfile);

    if (ConvertTo::isLocalhost(clientAddress))
        return true;

    Log::warn("testPoco::Net::IPAddress(\"" + clientAddress + "\") failed: ");
    Statement select(session);
    int count;
    select << "SELECT COUNT(*) FROM maciplist WHERE macip=? AND ftype='ip'",
                    into(count), use(clientAddress);
    try
    {
        while (!select.done())
        {
            select.execute();
            break;
        }
    }
    catch (Poco::Exception& e)
    {
        std::cerr << e.displayText() << std::endl;
        session.close();
        return false;
    }
    session.close();
    return count > 0;
}

/// @TODO: maybe no need
/// 定期清除資料
void ConvDB::cleanup()
{
    Session session("SQLite", dbfile);
    Statement insert(session);
    insert << "delete from keylist where (strftime('%s', 'now')-expires) > 60*60",
              now;
    insert << "VACUUM FULL", now;
    session.close();
    std::cout<<"ConvDB::cleanup"<<std::endl;
}

/// reponse http error code and msg
void ConvertTo::httpError(std::weak_ptr<StreamSocket> _socket,
                          Poco::Net::HTTPResponse& response,
                          Poco::Net::HTTPResponse::HTTPStatus errorCode,
                          const std::string msg)
{
    auto socket = _socket.lock();
    response.setStatusAndReason(errorCode, msg);
    response.setContentLength(0);
    socket->send(response);
    socket->shutdown();
}

/// http://server/lool/convert-to
/// called by LOOLWSD
void ConvertTo::handleConvertTo(std::weak_ptr<StreamSocket> _socket,
                             const Poco::Net::HTTPRequest& request,
                             Poco::MemoryInputStream& message)
{
    auto socket = _socket.lock();
    Application::instance().logger().setChannel(channel);

    HTTPResponse response;
    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.set("Access-Control-Allow-Headers",
        "Origin, X-Requested-With, Content-Type, Accept");

    std::string copy2, fromPath, macAddr;
    CConvertToPartHandler handler(fromPath);
    HTMLForm form(request, message, handler);
    if (form.has("mac_addr") && !form.get("mac_addr").empty())
    {
        macAddr = form.get("mac_addr");
    }

    if (!validateUri(response, request.getURI()))
    {
        httpError(_socket, response, HTTPResponse::HTTP_UNAUTHORIZED);
        return;
    }

    ConvDB *cdb = new ConvDB();
    cdb->setDbPath();

    auto access_token = form.has("access_token") ?
                            form.get("access_token") : "";
    std::cout<<"access_token:"<<access_token<<std::endl;

    if (access_token.empty() && macAddr.empty() &&
        !cdb->validateIP(socket->clientAddress()))
    {
        httpError(_socket, response,
                    HTTPResponse::HTTP_UNAUTHORIZED,
                    "ip address not allow");
        return;
    }

    StringTokenizer tokens(request.getURI(), "/?");
    const auto format = tokens[3];
    const bool queue = tokens[4] == "fileurl";

    if (access_token.empty())
    {
        if (!macAddr.empty())
        {
            // validate mac address
            if (!cdb->validateMac(macAddr))
            {
                httpError(_socket, response,
                            HTTPResponse::HTTP_UNAUTHORIZED,
                            "mac address not allow");
                return;
            }
        }
        try
        {
            auto buf = cdb->newkey();
            std::cout<<"newkey:"<<buf<<std::endl;

            response.setContentLength(buf.length());
            socket->send(response);
            socket->send(buf.c_str(), buf.length(), true);
            socket->shutdown();
            return;
        }
        catch (Poco::Exception& e)
        {
            std::cerr << e.displayText() << std::endl;
            httpError(_socket, response,
                        HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                        "db access error");
            return;
        }
    }
    else
    {
        if (!cdb->hasKey(access_token))
        {
            httpError(_socket, response,
                        HTTPResponse::HTTP_BAD_REQUEST,
                        "wrong key: key not found");
            return;
        }
        if (cdb->hasKeyExpired(access_token))
        {
            httpError(_socket, response,
                        HTTPResponse::HTTP_BAD_REQUEST,
                        "wrong key: key has expired");
            return;
        }
        if (queue)
        {
            Poco::UUIDGenerator& gen =
                Poco::UUIDGenerator::defaultGenerator();
            copy2 = "/data/oxooldocs/" +
                    gen.create().toString() + "." + format;
            cdb->setFile(access_token, copy2);
        }
    }
    delete cdb;

    Process::PID pid = fork();
    if (pid < 0)
    {
        httpError(_socket, response,
                    HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                    "error loading convertto");
        _exit(Application::EXIT_SOFTWARE);
        return;
    }
    else if (pid == 0)
    {
        if ((pid = fork()) < 0)
        {
            httpError(_socket, response,
                        HTTPResponse::HTTP_SERVICE_UNAVAILABLE,
                        "error loading convertto");
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

            std::string waTitleIn, waAngleIn, waColorIn, waOpacityIn;

            waTitleIn = form.has("watermarkTitle") ?
                            form.get("watermarkTitle") : "";
            waAngleIn = form.has("watermarkAngle") ?
                            form.get("watermarkAngle"): "";
            waColorIn = form.has("watermarkColor") ?
                            form.get("watermarkColor") : "";
            waOpacityIn = form.has("watermarkOpacity") ?
                            form.get("watermarkOpacity") : "";

            if (form.has("useTextWatermark") && form.get("useTextWatermark") == "1")
                setWatermarkParams(waTitleIn, waAngleIn,
                                    waColorIn, waOpacityIn);
            if (format == "pdf")
            {
                fromPath = outputODF(fromPath, "", "odt");
                setWatermarkParams("", "", "", "");
            }

            auto outfile = outputODF(fromPath, copy2, format);
            //if (outfile.empty())
            //    std::cout << "outfile empty()" << std::endl;

            if (!outfile.empty())
            {
                if (!format.empty())
                {
                    LOG_INF("Conversion request for URI [" << fromPath << "].");

                    if (!queue)
                    {
                        auto mimeType = getMimeType();
                        std::cout << "mimetype: " << mimeType << std::endl;
                        HttpHelper::sendFile(socket, outfile,
                                             mimeType, response);
                        //std::cout << "outputfile: " << outfile << std::endl;
                        Poco::File(outfile).remove(true);
                        std::cout << "remove: " << outfile << std::endl;
                    }
                    else
                    {
                        auto msg = getMessage();
                        std::string buf = "[\"file://" + copy2 + "\", \"" + msg  + "\"]";

                        response.setContentLength(buf.length());
                        socket->send(response);
                        socket->send(buf.c_str(), buf.length(), true);
                        socket->shutdown();
                    }
                }
            }
            else
            {
                response.setStatusAndReason(
                    HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "轉檔失敗");
                response.setContentLength(0);
                socket->send(response);
                socket->shutdown();
            }
            std::cout << "convert-to: shutdown"<<getpid()<<std::endl;

            _exit(Application::EXIT_SOFTWARE);
            return;
        }
    }
    else
    {
        std::cout << "call from parent" << std::endl;
        waitpid(pid, NULL, 0); // 父程序呼叫waitpid(), 等待子程序終結,並捕獲返回狀態
    }
    return;
}
