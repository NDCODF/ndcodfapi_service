#ifndef __TBL2SC_H__
#define __TBL2SC_H__
#include "config.h"
#include "Socket.hpp"
#include <sys/wait.h>

#include <Poco/MemoryStream.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Process.h>

using Poco::Process;

class Tbl2SC
{
public:
    Tbl2SC();

    virtual bool isTbl2SCUri(std::string);
    virtual void doConvert(const Poco::Net::HTTPRequest&,
                           Poco::MemoryInputStream&,
                           std::weak_ptr<StreamSocket>);
    virtual void setProgPath(std::string path)
    {
        loPath = path + "/program";
    }

private:
    int mimetype;
    std::string loPath;  // soffice program path

    std::string getMimeType();
    bool validate(const Poco::Net::HTMLForm&,
                  std::weak_ptr<StreamSocket>);
    std::string outputODF(const std::string, const std::string);
};
#endif
