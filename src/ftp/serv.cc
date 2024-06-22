#include <iostream>
#include <crow.h>
#include <sys/locals.h>
#include <sys/log.h>

enum eFileType
{
    StyleSheet,
    JavaScript,
    Json,
    Python,
    Other
};

static std::map<eFileType, std::string> fileTypes {
    {eFileType::StyleSheet, "text/css"},
    {eFileType::JavaScript, "application/javascript"},
    {eFileType::Json, "application/json"},
    {eFileType::Python, "text/x-python"},
    {eFileType::Other, "text/plain"}
};

const eFileType EvaluateFileType(std::filesystem::path filePath)
{
    const std::string extension = filePath.extension().string();

    if      (extension == ".css")  { return eFileType::StyleSheet; }
    else if (extension == ".js")   { return eFileType::JavaScript; }
    else if (extension == ".json") { return eFileType::Json; }
    else if (extension == ".py")   { return eFileType::Python; }

    else
    {
        return eFileType::Other;
    }
}

const bool IsInternalRequest(const std::filesystem::path& filepath) 
{
    for (const auto& component : filepath) 
    {
        return component.string() == "_internal_";
    }

    return {};
}

namespace Crow
{
    struct ResponseProps
    {
        std::string contentType;
        std::string content;
    };

    ResponseProps EvaluateRequest(std::filesystem::path path)
    {
        eFileType fileType = EvaluateFileType(path.string());
        const std::string contentType = fileTypes[fileType];

        return {
            contentType,
            SystemIO::ReadFileSync(path.string())
        };
    }

    crow::response HandleRequest(std::string path)
    {
        crow::response response;
        std::filesystem::path absolutePath;

        if (IsInternalRequest(path))
        {
            const auto relativePath = std::filesystem::relative(fmt::format("/{}", path), "/_internal_");
            absolutePath = SystemIO::GetSteamPath() / "ext" / "data" / relativePath;
        }
        else
        {
            absolutePath = SystemIO::GetSteamPath() / "plugins" / path;
        }

        Logger.Log("requesting file: {}", absolutePath.string());
        ResponseProps responseProps = EvaluateRequest(absolutePath);

        response.add_header("Content-Type", responseProps.contentType);
        response.add_header("Access-Control-Allow-Origin", "*");
        response.write(responseProps.content);

        return response;
    }

    void RunMainLoop()
    {
        crow::SimpleApp app;
        app.server_name("millennium/1.0");
        app.loglevel(crow::LogLevel::Critical);

        CROW_ROUTE(app, "/<path>")(HandleRequest);
        app.port(12033).multithreaded().run();
    }

    void CreateAsyncServer()
    {
        std::thread(RunMainLoop).detach();
    }
}