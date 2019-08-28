#ifndef __MERGEODF_H__
#define __MERGEODF_H__
#include "config.h"
#include "Socket.hpp"

#include <Poco/Tuple.h>
#include <Poco/FileStream.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/MemoryStream.h>
#include <Poco/URI.h>
#include <Poco/DOM/Element.h>
#include <Poco/DOM/AutoPtr.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/UUID.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/Process.h>
#include <Poco/Util/LayeredConfiguration.h>
#include <Poco/JSON/Object.h>

using Poco::Path;
using Poco::URI;
using Poco::XML::AutoPtr;
using Poco::Net::NameValueCollection;
using Poco::Net::HTTPRequest;
using Poco::Net::HTMLForm;
using Poco::XML::Node;
using Poco::XML::NodeList;
using Poco::XML::Element;
using Poco::MemoryInputStream;
using Poco::Process;
using Poco::JSON::Object;
typedef Poco::Tuple<std::string, std::string> VarData;


/*
 * 紀錄 log 用:
 * 將 log 紀錄到 sqlite, 可用來查詢呼叫次數
 */
class LogDB
{
public:
    LogDB();
    /*virtual*/ ~LogDB();

    void setDbPath();

    /// set endpoint
    void setApi(std::string Api)
    {
        api = Api;
    }

    void notice(std::weak_ptr<StreamSocket>,
                 Poco::Net::HTTPResponse&,
                 std::string);
    int getAccessTimes();

private:
    std::string api;
    std::string dbfile;
};


class Parser
{
public:
    enum class DocType
    {
        OTHER,
        TEXT,
        SPREADSHEET
    };


    Parser(std::string);
    Parser(URI&);

    ~Parser();

    std::string getMimeType();
    void extract(std::string);

    std::string jsonVars();
    std::string jjsonVars();
    std::string yamlVars();

    std::vector<std::list<Element*>> scanVarPtr();
    std::string zipback();

    void updatePic2MetaXml();

    bool isValid();

    void setOutputFlags(bool, bool);
    std::string varKeyValue(std::string, std::string);
    void setSingleVar(Object::Ptr, std::list<Element*> &);
    void setGroupVar(Object::Ptr, std::list<Element*> &);
    std::string jsonvars;   // json 說明 - openapi
    std::string jjsonvars;  // json 範例
    std::string yamlvars;   // yaml
private:
    DocType doctype;
    bool success;
    unsigned picserial;

    bool outAnotherJson;
    bool outYaml;

    std::map < std::string, Path > zipfilepaths;
    AutoPtr<Poco::XML::Document> docXML;
    std::list<Element*> groupAnchorsSc;

    std::string extra2;
    std::string contentXml;
    std::string contentXmlFileName;
    std::string metaFileName;

    void detectDocType();
    bool isText();
    bool isSpreadSheet();

    std::string replaceMetaMimeType(std::string);
    void updateMetaInfo();

    std::string parseEnumValue(std::string, std::string, std::string);
    std::string parseJsonVar(std::string, std::string, bool, bool);

    const std::string PARAMTEMPL = R"MULTILINE(
                    "%s": {
                        "type": "%s"%s
                    })MULTILINE";
    const std::string PARAMGROUPTEMPL = R"MULTILINE(
                      "%s": {
                        "type": "array",
                        "xml": {
                            "name": "%s",
                            "wrapped": true
                        },
                        "items": {
                          "type": "object",
                          "properties": {
                            %s
                          }
                        }
                      },)MULTILINE";
    const std::string YAMLPARAMTEMPL = R"MULTILINE(              "%s":
                "type": "%s"
%s)MULTILINE";
    const std::string YAMLPARAMGROUPTEMPL = R"MULTILINE(              "%s":
                "type": "array"
                "xml":
                  "name": "%s"
                  "wrapped": true
                "items":
                  "type": "object"
                  "properties":
%s
)MULTILINE";
};


// @TODO: 怪？設定這個變數值以後，lool stop 就會 double free error!
//const std::string Parser::TAG_VARDATA_SC = "office:target-frame-name";

class MergeODF
{
public:
    MergeODF();

    enum class MergeStatus
    {
        OK,
        PARAMETER_REQUIRE,  /// @TODO: unused?
        TEMPLATE_NOT_FOUND,
        JSON_PARSE_ERROR
    };

    virtual void setProgPath(std::string path)
    {
        loPath = path + "/program";
    }

    virtual void setLogPath(std::string);

    virtual std::string isMergeToUri(std::string,
                                     bool forHelp=false,
                                     bool anotherJson=false,
                                     bool yaml=false);
    virtual std::string isMergeToHelpUri(std::string,
                                         bool anotherJson=false,
                                         bool yaml=false);
    virtual std::string isMergeToQueryAccessTime(std::string);
    virtual std::string makeApiJson(std::string,
                                    bool anotherJson=false,
                                    bool yaml=false,
                                    bool showHead=true);
    virtual void handleMergeTo(std::weak_ptr<StreamSocket>,
                               const Poco::Net::HTTPRequest&,
                               Poco::MemoryInputStream&);
    virtual int getApiCallTimes(std::string);
    virtual void responseAccessTime(std::weak_ptr<StreamSocket>, std::string);

private:
    LogDB *logdb;
    std::string loPath;  // soffice program path

    std::string mimetype;
    MergeStatus mergeStatus;

    std::string getMimeType();
    std::string getDocExt();
    MergeStatus getMergeStatus();
    std::string doMergeTo(const HTTPRequest&, MemoryInputStream&);

    std::list<std::string> getTemplLists(bool);
    std::string getContentFromTemplFile(std::string);
    std::list<std::string> getVarsFromTempl(std::string, bool);
    std::list<std::string> getVarsFromUri(std::string);
    std::string keyword2Lower(std::string, std::string);
    bool parseJson(HTMLForm &);
    Object::Ptr parseArray2Form(HTMLForm &);

    std::string outputODF(std::string);

    const std::string TEMPLH = R"MULTILINE(
{
    "swagger": "2.0",
    "info": {
        "version": "v1",
        "title": "ODF 報表 API",
        "description": ""
    },
    "host": "%s",
    "paths": {
        %s
    },
    "schemes": [
        "http",
        "https"
    ],
    "parameters": {
        "outputPDF": {
            "in": "query",
            "name": "outputPDF",
            "required": false,
            "type": "boolean",
            "allowEmptyValue": true,
            "description": "轉輸出成 PDF 格式"
        }
    }
}
)MULTILINE";

    const std::string YAMLTEMPLH = R"MULTILINE(swagger: '2.0'
info:
  version: v1
  title: ODF 報表 API
  description: ''
host: %s
paths:%s
schemes: ["http", "https"]
parameters:
  outputPDF:
    in: query
    name: outputPDF
    required: false
    type: boolean
    allowEmptyValue : true
    description: 轉輸出成 PDF 格式)MULTILINE";

    const std::string APITEMPL = R"MULTILINE(
        "/lool/merge-to/%s/accessTime": {
          "get": {
            "consumes": [
              "multipart/form-data",
              "application/json"
            ],
            "responses": {
              "200": {
                "description": "傳送成功",
                "schema": {
                  "type": "object",
                  "properties": {
                    "call_time": {
                      "type": "integer",
                      "description": "呼叫次數."
                    }
                  }
                }
              }
            }
          }
        },
        "/lool/merge-to/%s": {
            "post": {
                "consumes": [
                    "multipart/form-data",
                    "application/json"
                ],
            "parameters": [
              {
                "$ref": "#/parameters/outputPDF"
              },
              {
                "in": "body",
                "name": "body",
                "description": "",
                "required": true,
                "schema": {
                    "type": "object",
                    "properties": {%s}
                    }
              }
            ],
            "responses": {
              "200": {
                "description": "傳送成功"
              },
              "401": {
                "description": "Json data error"
              },
              "503": {
                "description": "merge error / error loading mergeodf"
              }
            }
          }
        }
)MULTILINE";

    const std::string YAMLTEMPL = R"MULTILINE(
  /lool/merge-to/%s/accessTime:
    get:
      consumes:
        - application/json
      responses:
        '200':
          description: 傳送成功
          schema:
            type: object
            properties:
              call_time:
                type: integer
                description: 呼叫次數.
  /lool/merge-to/%s:
    post:
      consumes:
        - multipart/form-data
        - application/json
      parameters:
        - $ref: '#/parameters/outputPDF'
        - in: body
          name: body
          description: ''
          required: false
          schema:
            type: object
            properties:
%s      responses:
        '200':
          description: 傳送成功
        '401':
          description: 'Json data error'
        '503':
          description: 'merge error / error loading mergeodf'
)MULTILINE";
};

#endif
