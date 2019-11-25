
#ifndef __templaterepo_H__
#define __templaterepo_H__
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

std::list<std::string> templLists(bool);
std::string zip_ALL_FILE();
std::string zip_DIFF_FILE(Object::Ptr);
Object::Ptr JSON_FROM_FILE();
void createDirectroy(std::string);

class TemplateRepo
{
public:
    TemplateRepo();
    virtual ~TemplateRepo();
    virtual void doTemplateRepo(std::weak_ptr<StreamSocket>, const Poco::Net::HTTPRequest& , Poco::MemoryInputStream&);

    virtual void getInfoFile(std::weak_ptr<StreamSocket>);
    virtual void syncTemplates(std::weak_ptr<StreamSocket>, const Poco::Net::HTTPRequest& , Poco::MemoryInputStream&);
    virtual void downloadAllTemplates(std::weak_ptr<StreamSocket>);
    virtual std::string makeApiJson(std::string,
                                    bool anotherJson=false,
                                    bool yaml=false,
                                    bool showHead=true);
    virtual bool isTemplateRepoHelpUri(std::string);
    virtual void handleAPIHelp(const Poco::Net::HTTPRequest&,std::weak_ptr<StreamSocket>);
private:
    const std::string YAMLTEMPL = R"MULTILINE(
swagger: '2.0'
info:
  version: v1
  title: ODF Template Center API
  description: ''
host: '%s'
paths:
  /lool/templaterepo/list:
    get:
      responses:
        '200':
          description: Success
          schema:
            type: object
            properties:
              Category:
                type: array
                items: 
                  $ref: '#/definitions/Category'
  /lool/templaterepo/sync:
    post:
      consumes:
        - application/json
      parameters:
        - $ref : '#/parameters/Sync'
      responses:
        '200':
          description: Success
        '401':
          description: Json data error
schemes:
  - http
  - https
definitions:
  Category:
    type: object
    required:  
      - uptime
      - endpt
      - cid
      - hide
      - extname
      - docname
    properties:
      uptime:
        type: string
        format: date-time
      endpt:
        type: string
      cid:
        type: string
      hide:
        type: string
      extname:
        type: string
      docname:
        type: string
        
parameters:
  Sync:
    in: body
    name: body
    description: ''
    required: true
    schema:
      type: object
      properties:
        Category:
          type: array
          items: 
            $ref: '#/definitions/Category'

)MULTILINE";
};

#endif
