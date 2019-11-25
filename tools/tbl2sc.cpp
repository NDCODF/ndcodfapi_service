#include "tbl2sc.h"

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <Poco/Util/ServerApplication.h>
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
#include <Poco/Data/Statement.h>
#include <Poco/Net/HTTPResponse.h>
#include <stdlib.h>
#include <iostream>
#include <assert.h>
#include <Poco/DOM/AutoPtr.h>
#include <Poco/FileStream.h>
#include <Poco/Exception.h>


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
using Poco::XML::AutoPtr;
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
using Poco::FileOutputStream;
using Poco::IOException;
using Poco::Util::Application;

extern "C" Tbl2SC* create_object()
{
    return new Tbl2SC;
}

Tbl2SC::Tbl2SC()
{}

/// check if uri valid this ap.
bool Tbl2SC::isTbl2SCUri(std::string uri)
{
    StringTokenizer tokens(uri, "/?");
    return (tokens.count() == 3 && tokens[2] == "table2spreadsheet");
}

/// mimetype
std::string Tbl2SC::getMimeType()
{
    switch (mimetype)
    {
        case LOK_DOCTYPE_SPREADSHEET:
            return "application/vnd.oasis.opendocument.spreadsheet";
        default:
            return "application/pdf";
    }
}

/// validate for form args
bool Tbl2SC::validate(const HTMLForm &form,
        std::weak_ptr<StreamSocket> _socket)
{
    HTTPResponse response;
    response.set("Access-Control-Allow-Origin", "*");
    response.set("Access-Control-Allow-Methods", "POST, OPTIONS");
    response.set("Access-Control-Allow-Headers",
            "Origin, X-Requested-With, Content-Type, Accept");

    auto socket = _socket.lock();

    if (!form.has("format"))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                "parameter format must assign");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    const std::string format = form.get("format");
    if (format != "ods" && format != "pdf")
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                "wrong parameter format");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    if (!form.has("title"))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                "parameter title must assign");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    if (!form.has("content"))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                "parameter content must assign");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    const std::string title = form.get("title");
    std::string content = form.get("content");

    Poco::RegularExpression re(".*<table [^<]*>.*</table>.*",
            Poco::RegularExpression::RE_DOTALL);
    if (!re.match(content))
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                "no <table>");
        response.setContentLength(0);
        socket->send(response);
        socket->shutdown();
        return false;
    }
    return true;
}

/// convert using soffice.so
std::string Tbl2SC::outputODF(const std::string odffile,
        const std::string format)
{
    if (odffile.empty())
    {
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
    lok::Document * lodoc = llo->documentLoad(odffile.c_str(), options);
    if (!lodoc)
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to load document (" << errmsg << ")" << std::endl;
        return "";
    }

    //Run Macro
    lodoc->postUnoCommand("macro:///Tbl2sc.tools.SetOptimalColumn()");

    outfile = odffile + ".odf";
    //std::cout << outfile << std::endl;
    if (!lodoc->saveAs(outfile.c_str(), format.c_str(), options))
    {
        const char * errmsg = llo->getError();
        std::cerr << ": LibreOfficeKit failed to export (" << errmsg << ")" << std::endl;

        //Poco::File(outfile).remove(true);
        //std::cout << "remove: " << odffile << std::endl;

        delete lodoc;
        return "";
    }
    if (format == "pdf")
        mimetype = -1;
    else
    {
        lodoc = llo->documentLoad(outfile.c_str(), options);
        if (!lodoc)
            mimetype = -1;
        else
            mimetype = lodoc->getDocumentType();
    }

    return outfile;
}

// XML 操作

/// 將 ODF 檔案解開
std::tuple<std::string, std::string> extract(std::string templfile)
{
    /*
     * templfile : 要解開的 ODF 檔案
     */
    std::string extra2 = TemporaryFile::tempName();

    std::ifstream inp(templfile, std::ios::binary);
    assert (inp.good());
    Decompress dec(inp, extra2);
    dec.decompressAllFiles();
    assert (!dec.mapping().empty());

    std::map < std::string, Path >  zipfilepaths = dec.mapping();
    std::string contentXmlFileName, metaFileName;
    for (auto it = zipfilepaths.begin(); it != zipfilepaths.end(); ++it)
    {
        const auto fileName = it->second.toString();
        if (fileName == "content.xml")
            contentXmlFileName = extra2 + "/" + fileName;

        if (fileName == "META-INF/manifest.xml")
            metaFileName = extra2 + "/" + fileName;
    }

    return std::make_tuple(contentXmlFileName, extra2);
}

void saveXmlBack(AutoPtr<Poco::XML::Document> docXML,
        std::string xmlfile)
{
    /*
     * docXML : Poco 讀出來的 XML 檔案
     * xmlfile : XML 的檔案名稱
     */
    std::ostringstream ostrXML;
    DOMWriter writer;
    writer.writeNode(ostrXML, docXML);
    const auto xml = ostrXML.str();

    Poco::File f(xmlfile);
    f.setSize(0);  // truncate

    Poco::FileOutputStream fos(xmlfile, std::ios::binary);
    fos << xml;
    fos.close();
}
std::string zipback(std::string extra2, AutoPtr<Poco::XML::Document> docXML,
        std::string contentXmlFileName)
{   
    /*
     * extra2 : 要壓縮的資料夾根目錄路徑
     * docXML : Poco 讀出來的 XML 檔案
     * contentXmlFileName : 一般來說就是 ODF 檔案解出來的 content.xml 的路徑
     * 
     */
    saveXmlBack(docXML, contentXmlFileName);

    // zip  
    const auto zip2 = extra2 + ".ods";
    std::cout << "zip2: " << zip2 << std::endl;

    std::ofstream out(zip2, std::ios::binary);
    Compress c(out, true);

    c.addRecursive(extra2);
    c.close();
    return zip2;
}


std::string customizeSC(std::string filepath, std::string font, std::string oddRowColor)
{
    // Check optional value of the font and oddRowColor
    if(font == "TimesNewRoman")
        font = "Times New Roman";
    if(font != "Times New Roman" && font!= "微軟正黑體" && font!="標楷體")
        font = "微軟正黑體";
    if(oddRowColor == "")
        oddRowColor = "#9d9d9d";

    // load the xml file to program
    std::tuple<std::string, std::string> unzipPath = extract(filepath);
    auto contentXmlFilePath = std::get<0>(unzipPath);
    auto extraDirectory = std::get<1>(unzipPath);
    InputSource sourceFile(contentXmlFilePath);
    DOMParser parser;
    parser.setFeature(XMLReader::FEATURE_NAMESPACES, false);
    parser.setFeature(XMLReader::FEATURE_NAMESPACE_PREFIXES, true);
    AutoPtr<Poco::XML::Document> docXML;
    docXML = parser.parse(&sourceFile);

    auto style_deep = docXML->createElement("style:style");

    //Set style Node
    style_deep->setAttribute("style:name", "looldark");
    style_deep->setAttribute("style:family", "table-cell");
    style_deep->setAttribute("style:parent-style-name", "Default");

    //Set style:table-cell-properties
    auto style_cell_property = docXML->createElement("style:table-cell-properties");
    style_cell_property->setAttribute("fo:background-color", oddRowColor); // Assign rowColor here
    style_cell_property->setAttribute("fo:border", "0.06pt solid #000000");

    //Set style text-properties
    auto style_text_prop = docXML->createElement("style:text-properties");
    style_text_prop->setAttribute("style:font-name",font); // Assign font here
    style_text_prop->setAttribute("style:use-window-font-color","true");
    style_text_prop->setAttribute("style:text-outline","false");
    style_text_prop->setAttribute("style:text-line-through-style","none");
    style_text_prop->setAttribute("style:text-line-through-type","none");
    style_text_prop->setAttribute("fo:font-size","10pt");
    style_text_prop->setAttribute("fo:language","en");
    style_text_prop->setAttribute("fo:country","US");
    style_text_prop->setAttribute("fo:font-style","normal");
    style_text_prop->setAttribute("fo:text-shadow","none");
    style_text_prop->setAttribute("style:text-underline-style","none");
    style_text_prop->setAttribute("fo:font-weight","normal");
    style_text_prop->setAttribute("style:text-underline-mode","continuous");
    style_text_prop->setAttribute("style:text-overline-mode","continuous");
    style_text_prop->setAttribute("style:text-line-through-mode","continuous");
    style_text_prop->setAttribute("style:font-size-asian","10pt");
    style_text_prop->setAttribute("style:language-asian","zh");
    style_text_prop->setAttribute("style:country-asian","TW");
    style_text_prop->setAttribute("style:font-style-asian","normal");
    style_text_prop->setAttribute("style:font-weight-asian","normal");
    style_text_prop->setAttribute("style:font-size-complex","10pt");
    style_text_prop->setAttribute("style:language-complex","hi");
    style_text_prop->setAttribute("style:country-complex","IN");
    style_text_prop->setAttribute("style:font-style-complex","normal");
    style_text_prop->setAttribute("style:font-weight-complex","normal");
    style_text_prop->setAttribute("style:text-emphasize","none");
    style_text_prop->setAttribute("style:font-relief","none");
    style_text_prop->setAttribute("style:text-overline-style","none");
    style_text_prop->setAttribute("style:text-overline-color","font-color");

    //Add prop to style
    style_deep->appendChild(style_text_prop);
    style_deep->appendChild(style_cell_property);

    //Clone a new style to white bg
    auto style_light = docXML->createElement("style:style");
    style_light->setAttribute("style:name", "loollight");
    style_light->setAttribute("style:family", "table-cell");
    style_light->setAttribute("style:parent-style-name", "Default");
    
    auto light_style_cell = static_cast<Element*> (style_cell_property->cloneNode(true));
    auto light_style_text = style_text_prop->cloneNode(true);
    light_style_cell->setAttribute("fo:background-color", "#ffffff");
    style_light->appendChild(light_style_text);
    style_light->appendChild(light_style_cell);

    //Add new style to docXML
    auto styleNode = static_cast<Element *> (docXML->getElementsByTagName("office:automatic-styles")->item(0));
    styleNode->appendChild(style_deep);
    styleNode->appendChild(style_light);

    //Add style to cell
    auto rowList = docXML->getElementsByTagName("table:table-row");
    int rowLen = rowList->length();
    std::string styleName = "looldark";
    for ( int i = 0; i < rowLen; i++)
    {
        if(i%2 != 0)
            styleName = "loollight";
        else
            styleName = "looldark";
        auto elm = static_cast<Element *> (rowList->item(i));
        auto childs = elm->getElementsByTagName("table:table-cell");
        for(unsigned int j=0; j < childs->length();j++){
            auto child = static_cast<Element*>(childs->item(j));
            child->setAttribute("table:style-name", styleName);
        }
    }
    std::string outputPath = "";
    try{
        outputPath = zipback(extraDirectory, docXML, contentXmlFilePath);
    }    
    catch(Poco::IOException &e){
        std::cout<<e.displayText()<<std::endl;
        return outputPath;
    }
    return outputPath;
}
/// 轉檔:
/// <table>...</table>  轉成  ods or pdf
void Tbl2SC::doConvert(const Poco::Net::HTTPRequest& request,
        Poco::MemoryInputStream& message,
        std::weak_ptr<StreamSocket> _socket)
{
    HTMLForm form(request, message);
    if (!validate(form, _socket))
        return;

    const std::string format = form.get("format");
    const std::string title = form.get("title");
    const std::string content = form.get("content");
    const std::string font = form.get("font", "");
    const std::string oddRowColor = form.get("oddRowColor", "");


    const std::string templ = R"MULTILINE(
    <!doctype html>
    <html lang="zh-tw">
    <head>
        <title>%s</title>
        <meta charset="UTF-8">
        <meta http-equiv="X-UA-Compatible" content="IE=edge">
    </head>
    <body>
    %s
    </body>
    </html>
    )MULTILINE";


    auto buf = Poco::format(templ, title, content);
    //std::cout << buf <<std::endl;

    auto sourcefile = TemporaryFile::tempName() + ".xls";

    Poco::File f(sourcefile);
    f.createFile();

    Poco::FileOutputStream fos(sourcefile, std::ios::binary);
    fos << buf;
    fos.close();
    std::cout << sourcefile << std::endl;

    HTTPResponse response;
    auto socket = _socket.lock();

    Process::PID pid = fork();
    if (pid < 0)
    {
        response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST,
                "error running table2spreadsheet");
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
                "error loading table2spreadsheet");
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
            //outputfile is filepath located in /tmp/ generated by poco tempfile
            auto outputfile = outputODF(sourcefile, format);
            if (outputfile.empty())
            {
                response.setStatusAndReason(
                        HTTPResponse::HTTP_BAD_REQUEST, "convert error");
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }

            
            // customize the output file
            outputfile = customizeSC(outputfile, font, oddRowColor);
            if (outputfile.empty())
            {
                response.setStatusAndReason(
                        HTTPResponse::HTTP_BAD_REQUEST, "convert error");
                socket->send(response);
                socket->shutdown();
                _exit(Application::EXIT_SOFTWARE);
                return;
            }
            
            response.set("Access-Control-Allow-Origin", "*");
            response.set("Access-Control-Allow-Methods",
                    "POST, OPTIONS");
            response.set("Access-Control-Allow-Headers",
                    "Origin, X-Requested-With, Content-Type, Accept");

            HttpHelper::sendFile(socket, outputfile,
                    getMimeType(), response);

            Poco::File(outputfile).remove(true);
            std::cout << "remove: " << outputfile << std::endl;

            _exit(Application::EXIT_SOFTWARE);
        }
    }
    else
    {
        //std::cout << "call from parent" << std::endl;
        waitpid(pid, NULL, 0); // 父程序呼叫waitpid(), 等待子程序終結,並捕獲返回狀態
    }
}
