#ifndef __CONVERTTO_H__
#define __CONVERTTO_H__
#include "config.h"
#include "Socket.hpp"

#include <chrono>

#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/DOM/Element.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/DOM/AutoPtr.h>
#include <Poco/Util/LayeredConfiguration.h>
#include <Poco/MemoryStream.h>
#include <Poco/Process.h>

typedef Poco::Tuple<std::string, std::string> REPLACEDTAGS;

using Poco::XML::AutoPtr;
using Poco::XML::Node;
using Poco::XML::Element;
using Poco::Process;

class WatermarkParser
{
public:
    WatermarkParser();
    WatermarkParser(std::string);
    bool insertWaterMark(std::string, std::string,
                         std::string, std::string, std::string);

private:
    AutoPtr<Poco::XML::Document> styleXMLBase;
    AutoPtr<Poco::XML::Document> styleXML;
    std::list <REPLACEDTAGS> replacetags;

    std::string progPath;

    Element* createOurDummyTag(int);
    Element* parseStyleHeader(Element*);
    Node* parseTextP(Element*);
    bool parseDrawCustomShape(
            std::string,
            Node*,
            int,
            std::string,
            std::string);
    bool parseMgrXml(int, const std::string, const std::string);

    Node* getTextWatermarkXML(std::string);
    void setReplacedTags(AutoPtr<Element>, Node*);
    void updateXMLNS(AutoPtr<Poco::XML::Document>);
    void saveXmlBack(AutoPtr<Poco::XML::Document>, std::string);
};

class ConvDB
{
public:
    ConvDB();
    virtual ~ConvDB();

    bool hasKey(const std::string);
    bool hasKeyExpired(const std::string);
    virtual void setDbPath();

    void setFile(std::string, std::string);
    virtual std::string getFile(std::string);
    std::string newkey();

    bool validateIP(std::string);
    bool validateMac(std::string);

    virtual void cleanup();

private:
    std::string getKeyExpire(std::string);
    std::string dbfile;
    int keyTimeout;
};

class ConvertTo
{
public:
    ConvertTo();

    virtual void setProgPath(std::string path)
    {
        loPath = path + "/program";
    }

    virtual void setXmlPath(std::string path)
    {
        xmlPath = path;
    }

    virtual void setLogPath(std::string);
    virtual std::string makeApiJson(bool yaml=false, bool showHead=true);
    virtual void handleConvertTo(std::weak_ptr<StreamSocket>,
                                 const Poco::Net::HTTPRequest&,
                                 Poco::MemoryInputStream&);

    virtual bool isConvertTo(std::string);

    static bool isLocalhost(const std::string&);

private:
    AutoPtr<Poco::Channel> channel;

    void httpError(std::weak_ptr<StreamSocket>,
                   Poco::Net::HTTPResponse&,
                   Poco::Net::HTTPResponse::HTTPStatus errorCode,
                   const std::string msg="");
    void log(std::string, bool, std::chrono::steady_clock::time_point);
    std::string resolveIP(std::string);
    void setWatermarkParams(std::string, std::string,
                            std::string, std::string);
    bool validateUri(Poco::Net::HTTPResponse&, std::string);
    std::string outputODF(const std::string,
                          const std::string,
                          const std::string);

    std::string getMessage();
    std::string getMimeType();

    int mimetype;
    std::string workMessage;
    WatermarkParser waparser;
    std::string loPath;  // soffice program path
    std::string xmlPath;  // watermark 會參考到的 xml檔路徑
    std::string waTitle;
    std::string waAngle;
    std::string waColor;
    std::string waOpacity;

    const std::string TEMPLH = R"MULTILINE(
{
    "swagger": "2.0",
    "info": {
        "version": "v1",
        "title": "轉檔管理",
        "description": ""
    },
    "host": "%s",
    "paths": {
        %s
    }
}
)MULTILINE";

    const std::string YAMLTEMPLH = R"MULTILINE(swagger: '2.0'
info:
  version: v1
  title: 轉檔管理
  description: ''
host: %s
paths:%s)MULTILINE";

    const std::string TEMPL = R"MULTILINE(    "/lool/convert-to/odt/filecontent": {
      "post": {
        "summary": "convert to odt, return content",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          },
          {
            "in": "formData",
            "name": "useTextWatermark",
            "required": false,
            "type": "integer",
            "description": "是否使用文字型浮水印"
          },
          {
            "in": "formData",
            "name": "watermarkTitle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定文字"
          },
          {
            "in": "formData",
            "name": "watermarkAngle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定角度"
          },
          {
            "in": "formData",
            "name": "watermarkColor",
            "required": false,
            "type": "string",
            "format": "＃FFFFFF",
            "description": "文字型浮水印：設定顏色"
          },
          {
            "in": "formData",
            "name": "watermarkOpacity",
            "required": false,
            "type": "integer",
            "description": "文字型浮水印：設定透明度"
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    },
    "/lool/convert-to/ods/filecontent": {
      "post": {
        "summary": "convert to ods, return content",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    },
    "/lool/convert-to/odp/filecontent": {
      "post": {
        "summary": "convert to odp, return content",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    },
    "/lool/convert-to/odt/fileurl": {
      "post": {
        "summary": "convert to odt, return url",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          },
          {
            "in": "formData",
            "name": "useTextWatermark",
            "required": false,
            "type": "integer",
            "description": "是否使用文字型浮水印"
          },
          {
            "in": "formData",
            "name": "watermarkTitle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定文字"
          },
          {
            "in": "formData",
            "name": "watermarkAngle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定角度"
          },
          {
            "in": "formData",
            "name": "watermarkColor",
            "required": false,
            "type": "string",
            "format": "＃FFFFFF",
            "description": "文字型浮水印：設定顏色"
          },
          {
            "in": "formData",
            "name": "watermarkOpacity",
            "required": false,
            "type": "integer",
            "description": "文字型浮水印：設定透明度"
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    },
    "/lool/convert-to/ods/fileurl": {
      "post": {
        "summary": "convert to ods, return url",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    },
    "/lool/convert-to/odp/fileurl": {
      "post": {
        "summary": "convert to odp, return url",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    },
    "/lool/convert-to/pdf/filecontent": {
      "post": {
        "summary": "convert to pdf, return content",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          },
          {
            "in": "formData",
            "name": "useTextWatermark",
            "required": false,
            "type": "integer",
            "description": "是否使用文字型浮水印"
          },
          {
            "in": "formData",
            "name": "watermarkTitle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定文字"
          },
          {
            "in": "formData",
            "name": "watermarkAngle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定角度"
          },
          {
            "in": "formData",
            "name": "watermarkColor",
            "required": false,
            "type": "string",
            "format": "＃FFFFFF",
            "description": "文字型浮水印：設定顏色"
          },
          {
            "in": "formData",
            "name": "watermarkOpacity",
            "required": false,
            "type": "integer",
            "description": "文字型浮水印：設定透明度"
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    },
    "/lool/convert-to/pdf/fileurl": {
      "post": {
        "summary": "convert to pdf, return url",
        "consumes": [
          "multipart/form-data"
        ],
        "parameters": [
          {
            "in": "formData",
            "name": "access_token",
            "type": "string",
            "description": "key used for convert"
          },
          {
            "in": "formData",
            "name": "filename",
            "type": "file",
            "description": "The file to upload."
          },
          {
            "in": "formData",
            "name": "useTextWatermark",
            "required": false,
            "type": "integer",
            "description": "是否使用文字型浮水印"
          },
          {
            "in": "formData",
            "name": "watermarkTitle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定文字"
          },
          {
            "in": "formData",
            "name": "watermarkAngle",
            "required": false,
            "type": "string",
            "description": "文字型浮水印：設定角度"
          },
          {
            "in": "formData",
            "name": "watermarkColor",
            "required": false,
            "type": "string",
            "format": "＃FFFFFF",
            "description": "文字型浮水印：設定顏色"
          },
          {
            "in": "formData",
            "name": "watermarkOpacity",
            "required": false,
            "type": "integer",
            "description": "文字型浮水印：設定透明度"
          }
        ],
        "responses": {
          "200": {
            "description": "轉檔成功"
          },
          "401": {
            "description": "ip address not allow / mac address not allow"
          },
          "400": {
            "description": "wrong key: key not found / wrong key: key has expired"
          },
          "500": {
            "description": "轉檔失敗"
          },
          "503": {
            "description": "db access error / error loading convertto"
          }
        }
      }
    }
)MULTILINE";

    const std::string YAMLTEMPL = R"MULTILINE(
  /lool/convert-to/odt/filecontent:
    post:
      summary: 'convert to odt, return content'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
        - in: formData
          name: useTextWatermark
          required: false
          type: integer
          description: 是否使用文字型浮水印
        - in: formData
          name: watermarkTitle
          required: false
          type: string
          description: 文字型浮水印：設定文字
        - in: formData
          name: watermarkAngle
          required: false
          type: string
          description: 文字型浮水印：設定角度
        - in: formData
          name: watermarkColor
          required: false
          type: string
          format: ＃FFFFFF
          description: 文字型浮水印：設定顏色
        - in: formData
          name: watermarkOpacity
          required: false
          type: integer
          description: 文字型浮水印：設定透明度
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
  /lool/convert-to/ods/filecontent:
    post:
      summary: 'convert to ods, return content'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
  /lool/convert-to/odp/filecontent:
    post:
      summary: 'convert to odp, return content'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
  /lool/convert-to/odt/fileurl:
    post:
      summary: 'convert to odt, return url'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
        - in: formData
          name: useTextWatermark
          required: false
          type: integer
          description: 是否使用文字型浮水印
        - in: formData
          name: watermarkTitle
          required: false
          type: string
          description: 文字型浮水印：設定文字
        - in: formData
          name: watermarkAngle
          required: false
          type: string
          description: 文字型浮水印：設定角度
        - in: formData
          name: watermarkColor
          required: false
          type: string
          format: ＃FFFFFF
          description: 文字型浮水印：設定顏色
        - in: formData
          name: watermarkOpacity
          required: false
          type: integer
          description: 文字型浮水印：設定透明度
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
  /lool/convert-to/ods/fileurl:
    post:
      summary: 'convert to ods, return url'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
  /lool/convert-to/odp/fileurl:
    post:
      summary: 'convert to odp, return url'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
  /lool/convert-to/pdf/filecontent:
    post:
      summary: 'convert to pdf, return content'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
        - in: formData
          name: useTextWatermark
          required: false
          type: integer
          description: 是否使用文字型浮水印
        - in: formData
          name: watermarkTitle
          required: false
          type: string
          description: 文字型浮水印：設定文字
        - in: formData
          name: watermarkAngle
          required: false
          type: string
          description: 文字型浮水印：設定角度
        - in: formData
          name: watermarkColor
          required: false
          type: string
          format: ＃FFFFFF
          description: 文字型浮水印：設定顏色
        - in: formData
          name: watermarkOpacity
          required: false
          type: integer
          description: 文字型浮水印：設定透明度
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
  /lool/convert-to/pdf/fileurl:
    post:
      summary: 'convert to pdf, return url'
      consumes:
        - multipart/form-data
      parameters:
        - in: formData
          name: access_token
          type: string
          description: key used for convert
        - in: formData
          name: filename
          type: file
          description: The file to upload.
        - in: formData
          name: useTextWatermark
          required: false
          type: integer
          description: 是否使用文字型浮水印
        - in: formData
          name: watermarkTitle
          required: false
          type: string
          description: 文字型浮水印：設定文字
        - in: formData
          name: watermarkAngle
          required: false
          type: string
          description: 文字型浮水印：設定角度
        - in: formData
          name: watermarkColor
          required: false
          type: string
          format: ＃FFFFFF
          description: 文字型浮水印：設定顏色
        - in: formData
          name: watermarkOpacity
          required: false
          type: integer
          description: 文字型浮水印：設定透明度
      responses:
        '200':
          description: 轉檔成功
        '400':
          description: 'wrong key: key not found / wrong key: key has expired'
        '401':
          description: ip address not allow / mac address not allow
        '500':
          description: 轉檔失敗
        '503':
          description: db access error / error loading convertto
)MULTILINE";
};
#endif
