#include <stdafx.h>
#include <extern/injector/inject.hpp>
#include <include/config.hpp>
#include <extern/steam/cef_manager.hpp>
#include <extern/ipc/steamipc.hpp>

namespace websocket = boost::beast::websocket;
using namespace boost;

output_console console; skin_config config;
using tcp = boost::asio::ip::tcp;

nlohmann::basic_json<> skin_json_config;

/// <summary>
/// responsible for handling the client steam pages
/// </summary>
class client
{
private:
    //socket variables
    boost::asio::io_context io_context;
    boost::beast::websocket::stream<tcp::socket> socket;
    asio::ip::tcp::resolver resolver;

    //initialize the socket from class initializer
    client() : socket(io_context), resolver(io_context) {}

    mutable std::string sessionId, page_title;

    nlohmann::json socket_response;

    /// <summary>
    /// socket response errors
    /// </summary>
    class socket_response_error : public std::exception
    {
    public:
        enum errors {
            socket_error_message
        };

        socket_response_error(int errorCode) : errorCode_(errorCode) {}

        int code() const {
            return errorCode_;
        }

    private:
        int errorCode_;
    };

    /// <summary>
    /// function responsible for handling css and js injections
    /// </summary>
    /// <param name="title">the title of the cef page to inject into</param>
    inline const void patch(std::string title) noexcept
    {
        for (nlohmann::basic_json<>& patch : skin_json_config["Patches"])
        {
            bool contains_http = patch["MatchRegexString"].get<std::string>().find("http") != std::string::npos;
            //used regex match instead of regex find or other sorts, make sure you validate your regex 
            bool regex_match = std::regex_match(title, std::regex(patch["MatchRegexString"].get<std::string>()));

            if (contains_http or not regex_match)
                continue;

            console.imp("[client] match => " + std::string(title) + " regex: [" + patch["MatchRegexString"].get<std::string>() + "]");

            if (patch.contains("TargetCss")) steam_interface.evaluate(socket, patch["TargetCss"], steam_interface.script_type::stylesheet, socket_response);
            if (patch.contains("TargetJs"))  steam_interface.evaluate(socket, patch["TargetJs"], steam_interface.script_type::javascript, socket_response);
        }
    }

    /// <summary>
    /// cef instance created, captures remote and client based targets, however only local targets are controlled
    /// </summary>
    const void target_created() noexcept
    {
        socket.write(boost::asio::buffer(nlohmann::json({
            {"id", steam_cef_manager::response::attached_to_target},
            {"method", "Target.attachToTarget"},
            {"params", { {"targetId", socket_response["params"]["targetInfo"]["targetId"]}, {"flatten", true} } }
        }).dump()));
    }

    /// <summary>
    /// cef instance target info changed, happens when something ab the instance changes, i.e the client title or others
    /// </summary>
    const void target_info_changed() noexcept
    {
        //run a js vm to get cef info
        socket.write(boost::asio::buffer(
            nlohmann::json({
                {"id", steam_cef_manager::response::received_cef_details},
                {"method", "Runtime.evaluate"},
                {"sessionId", sessionId},
                {"params", {
                    //create an event to get the page title and url to use it later since it isnt evaluated properly on
                    //https://chromedevtools.github.io/devtools-protocol/tot/Target/#event-targetInfoChanged
                    {"expression", "(() => { return { title: document.title, url: document.location.href }; })()"},
                    {"returnByValue", true}
                }}
            }).dump()
        ));
    }

    /// <summary>
    /// received cef details from the js query, returns title and url
    /// </summary>
    const void received_cef_details()
    {
        if (socket_response.contains("error"))
        {
            //throw socket error to catch on the backend
            throw socket_response_error(socket_response_error::errors::socket_error_message);
        }

        page_title = socket_response["result"]["result"]["value"]["title"].get<std::string>();
        patch(page_title);

        //create a get document request, used to get query from the page, where further patching can happen
        socket.write(boost::asio::buffer(
            nlohmann::json({
                {"id", steam_cef_manager::response::get_document},
                {"method", "DOM.getDocument"},
                {"sessionId", sessionId}
            }).dump()
        ));
    }

    /// <summary>
    /// gets the document callback
    /// instantiated from the cef details callback 
    /// contains a json root of the query on the document, hard to parse all types
    /// </summary>
    const void get_doc_callback() noexcept
    {
        try {
            //get the <head> attributes of the dom which is what is most important in selecting pages
            std::string attributes = std::string(socket_response["result"]["root"]["children"][1]["attributes"][1]);

            //console.log(std::format("[FOR DEVELOPERS]\nselectable <html> query on page [{}]:\n -> [{}]\n", page_title, attributes));

            for (nlohmann::basic_json<>& patch : skin_json_config["Patches"])
            {
                bool contains_http = patch["MatchRegexString"].get<std::string>().find("http") != std::string::npos;

                if (contains_http || attributes.find(patch["MatchRegexString"].get<std::string>()) == std::string::npos)
                    continue;

                console.imp("[class_name] match => " + std::string(page_title) + " regex: [" + patch["MatchRegexString"].get<std::string>() + "]");

                if (patch.contains("TargetCss")) steam_interface.evaluate(socket, patch["TargetCss"], steam_interface.script_type::stylesheet, socket_response);
                if (patch.contains("TargetJs"))  steam_interface.evaluate(socket, patch["TargetJs"], steam_interface.script_type::javascript, socket_response);
            }

            /// <summary>
            /// inject millennium into the settings page, uses query instead of title because title varies between languages
            /// </summary>
            if (attributes.find("settings_SettingsModalRoot_") != std::string::npos) {
                steam_interface.inject_millennium(socket, socket_response);
            }
        }
        catch (std::exception& ex)
        {
            std::cout << "get_doc_callback exception: " << ex.what() << std::endl;
        }
    }

    /// <summary>
    /// received the sessionId from the cef instance after we attached to it
    /// we then use the sessionId provided to use runtime.evaluate on the instance
    /// </summary>
    const void attached_to_target() noexcept
    {
        sessionId = socket_response["result"]["sessionId"];
    }

public:
    /// <summary>
    /// event handler for when new windows are created/edited
    /// </summary>
    class cef_instance_created {
    public:
        /// <summary>
        /// singleton class instance for cross refrence
        /// </summary>
        /// <returns></returns>
        static cef_instance_created& get_instance() {
            static cef_instance_created instance;
            return instance;
        }

        using event_listener = std::function<void(nlohmann::basic_json<>&)>;

        //await event, remote sided
        void add_listener(const event_listener& listener) {
            listeners.push_back(listener);
        }

        //trigger change, client sided
        void trigger_change(nlohmann::basic_json<>& instance) {
            for (const auto& listener : listeners) {
                listener(instance);
            }
        }

    private:
        cef_instance_created() {}
        cef_instance_created(const cef_instance_created&) = delete;
        cef_instance_created& operator=(const cef_instance_created&) = delete;

        std::vector<event_listener> listeners;
    };

    //create a singleton instance of the class incase it needs to be refrences elsewhere and
    //only one instance needs to be running
    static client& get_instance()
    {
        static client instance;
        return instance;
    }

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    /// <summary>
    /// base derivative, handles all socket responses
    /// </summary>
    void handle_interface()
    {
        //connect to the browser websocket
        asio::connect(
            socket.next_layer(), 
            resolver.resolve(endpoints.debugger.host(), endpoints.debugger.port())
        );

        //create handshake on the remote url where the browser instance is stored
        socket.handshake(endpoints.debugger.host(), network::uri::uri(
            nlohmann::json::parse(steam_interface.discover(endpoints.debugger.string() + "/json/version"))
            ["webSocketDebuggerUrl"]
        ).path());

        //turn on discover targets from the cef browser instance
        socket.write(boost::asio::buffer(nlohmann::json({
            { "id", 1 },
            { "method", "Target.setDiscoverTargets" },
            { "params", {{"discover", true}} }
        }).dump()));

        //could make this async, but it doesnt matter, it only runs when a socket request is recieved
        while (true)
        {
            boost::beast::flat_buffer buffer; socket.read(buffer);
            socket_response = nlohmann::json::parse(boost::beast::buffers_to_string(buffer.data()));

            /// <summary>
            /// socket response associated with getting document details
            /// </summary>
            if (socket_response["id"] == steam_cef_manager::response::get_document) {
                try {
                    get_doc_callback();
                }
                //document contains no selectable query on the <head> 
                catch (const nlohmann::json::exception& e) {
                    continue;
                }
            }

            /// <summary>
            /// socket response called when a new CEF target is created with valid parameters
            /// </summary>
            if (socket_response["method"] == "Target.targetCreated" && socket_response.contains("params")) {
                target_created();
                client::cef_instance_created::get_instance().trigger_change(socket_response);
            }

            /// <summary>
            /// socket reponse called when a target cef instance changes in any way possible
            /// </summary>
            if (socket_response["method"] == "Target.targetInfoChanged" && socket_response["params"]["targetInfo"]["attached"]) {
                //trigger event used for remote handler
                target_info_changed();
                client::cef_instance_created::get_instance().trigger_change(socket_response);
            }

            /// <summary>
            /// socket response called when cef details have been received. 
            /// title, url and web debugger url
            /// </summary>
            if (socket_response["id"] == steam_cef_manager::response::received_cef_details) {  
                try {
                    received_cef_details();
                }
                catch (socket_response_error& ex) {
                    //response contains error message, we can discard and continue where we wait for a valid connection
                    if (ex.code() == socket_response_error::errors::socket_error_message) {
                        continue;
                    }
                }
            }

            /// <summary>
            /// socket response when attached to a new target, used to get the sessionId to use later
            /// </summary>
            if (socket_response["id"] == steam_cef_manager::response::attached_to_target) {
                attached_to_target();
            }
        }
    }
};

/// <summary>
/// responsible for handling the remote steam pages
/// </summary>
class remote
{
private:
    remote() {}
    /// <summary>
    /// used by the remote interface handler to check if the cef instance needs to be injected into
    /// </summary>
    /// <param name="patch">iteration of the patch from config</param>
    /// <param name="instance">cef instance</param>
    /// <returns>should patch</returns>
    std::vector<std::string> patched = {};

    inline bool __cdecl should_patch_interface(nlohmann::json& patch, const nlohmann::json& instance)
    {
        const std::string web_debugger_url = instance["webSocketDebuggerUrl"].get<std::string>();
        //check if the current instance was already patched
        for (const std::string& element : patched) {
            if (element == web_debugger_url) {
                return false;
            }
        }

        //get the url of the page we are active on 
        std::string steam_page_url_header = instance["url"].get<std::string>();

        if (std::regex_match(steam_page_url_header, std::regex(patch["MatchRegexString"].get<std::string>())))
        {
            constexpr const std::string_view exclude = "steamloopback.host";

            if (steam_page_url_header.find(exclude.data()) not_eq std::string::npos)
                return false;

            console.imp("[remote] match => " + steam_page_url_header + " regex: [" + patch["MatchRegexString"].get<std::string>() + "]");
            //mark that it was successfully patched
            patched.push_back(web_debugger_url);
            return true;
        }
        else return false;
    }

    /// <summary>
    /// set page settings using the chrome dev tools protocol
    /// </summary>
    /// <param name=""></param>
    /// <returns></returns>
    const void page_settings(boost::beast::websocket::stream<tcp::socket>& socket) noexcept {
        //remove CSP on the remote page to allow cross origin scripting, specifically
        //from the steamloopbackhost
        //https://chromedevtools.github.io/devtools-protocol/
        socket.write(boost::asio::buffer(
            nlohmann::json({
                {"id", 8},
                {"method", "Page.setBypassCSP"},
                {"params", {{"enabled", true}}}
            }).dump()
        ));
        //enable page event logging
        socket.write(boost::asio::buffer(nlohmann::json({ {"id", 1}, {"method", "Page.enable"} }).dump()));
        //reload page
        socket.write(boost::asio::buffer(nlohmann::json({ {"id", 1}, {"method", "Page.reload"} }).dump()));
    }

    /// <summary>
    /// handle remote interface, that are not part of the steam client.
    /// </summary>
    /// <param name="page">cef page instance</param>
    /// <param name="css_to_evaluate"></param>
    /// <param name="js_to_evaluate"></param>
    const void patch(const nlohmann::json& page, std::string css_eval, std::string js_eval)
    {
        boost::asio::io_context io_context;
        boost::beast::websocket::stream<tcp::socket> socket(io_context);
        asio::ip::tcp::resolver resolver(io_context);

        boost::network::uri::uri socket_url(page["webSocketDebuggerUrl"].get<std::string>());
        boost::asio::connect(socket.next_layer(), resolver.resolve(endpoints.debugger.host(), endpoints.debugger.port()));

        //connect to the socket associated with the page directly instead of using browser instance.
        //this way after the first injection we can inject immediatly into the page because we dont need to wait for the title 
        //of the remote page, we can just inject into it in a friendly way
        socket.handshake(endpoints.debugger.host(), socket_url.path());

        //activiate page settings needed
        page_settings(socket);

        //evaluate javascript and stylesheets
        std::function<void()> evaluate_scripting = ([&]() -> void {
            if (not js_eval.empty())
                steam_interface.evaluate(socket, js_eval, steam_interface.script_type::javascript);
            if (not css_eval.empty())
                steam_interface.evaluate(socket, css_eval, steam_interface.script_type::stylesheet);
        });
        //try to evaluate immediatly in case of slow initial connection times,
        //ie millennium missed the original page events so it misses the first inject
        evaluate_scripting();

        const auto async_read_socket = [&](
            boost::beast::websocket::stream<tcp::socket>& socket, 
            boost::beast::multi_buffer& buffer, 
            std::function<void(const boost::system::error_code&, std::size_t)> handler)
        {
            //consume the buffer to reset last response
            buffer.consume(buffer.size());
            //hang async on socket for a response
            socket.async_read(buffer, handler);
        };

        boost::beast::multi_buffer buffer;

        //handle reading from the socket asynchronously and continue processing
        std::function<void(const boost::system::error_code&, std::size_t)> handle_read = 
            [&](const boost::system::error_code& socket_error_code, std::size_t bytes_transferred) 
        {
            if (socket_error_code) {
                console.err("socket failure, couldnt read from socket");
                async_read_socket(socket, buffer, handle_read);
                return;
            }
            //get socket response and parse as a json object
            nlohmann::basic_json<> response = nlohmann::json::parse(boost::beast::buffers_to_string(buffer.data()));

            if (response.value("method", std::string()) != "Page.frameResized")
            {
                //evaluate scripts over and over again until they succeed, because sometimes it doesnt.
                //usually around 2 iterations max
                while (true)
                {
                    //evaluating scripting
                    evaluate_scripting();
                    //create a new response buffer but on the same socket stream to prevent shared memory usage
                    boost::beast::multi_buffer buffer; socket.read(buffer);
                    //the js evaluation was successful so we can break.
                    if (nlohmann::json::parse(boost::beast::buffers_to_string(buffer.data()))
                        ["result"]["exceptionDetails"]["exception"]["className"] not_eq "TypeError") {
                        break;
                    }
                }
            }
            //reopen socket async reading
            async_read_socket(socket, buffer, handle_read);
        };
        async_read_socket(socket, buffer, handle_read);
        io_context.run();
    }

public:
    //create a singleton instance of the class incase it needs to be refrences elsewhere and
    //only one instance needs to be running
    static remote& get_instance()
    {
        static remote instance;
        return instance;
    }

    remote(const remote&) = delete;
    remote& operator=(const remote&) = delete;

    const void handle_interface() noexcept 
    {
        //called when a new cef window is instantiated, used to check if its a remote page
        client::cef_instance_created::get_instance().add_listener([=](nlohmann::basic_json<>& instance) {

            //CEF instance information stored in instance["params"]["targetInfo"]
            const std::string instance_url = instance["params"]["targetInfo"]["url"].get<std::string>();

            //check if the updated CEF instance is remotely hosted.
            if (instance_url.find("http") == std::string::npos || instance_url.find("steamloopback.host") != std::string::npos)
                return;

            //check if there is a valid skin currently active
            if (skin_json_config["config_fail"])
                return;

            nlohmann::json instances = nlohmann::json::parse(steam_interface.discover(endpoints.debugger.string() + "/json"));
            std::vector<std::thread> threads;

            // Iterate over instances and addresses to create threads
            for (nlohmann::basic_json<>& instance : instances)
            {
                for (nlohmann::basic_json<>& address : skin_json_config["Patches"])
                {
                    // Check if the patch address is a URL (remote) and should be patched
                    if (address["MatchRegexString"].get<std::string>().find("http") == std::string::npos || !should_patch_interface(address, instance))
                        continue;

                    // Create a new thread and add it to the vector
                    threads.emplace_back([=, &remoteInstance = remote::get_instance()]() {
                        try {
                            // Use CSS and JS if available
                            remoteInstance.patch(instance, address.value("TargetCss", std::string()), address.value("TargetJs", std::string()));
                        }
                        catch (const boost::system::system_error& ex) {
                            std::cout << ex.what() << std::endl;
                            if (ex.code() == boost::asio::error::misc_errors::eof) {
                                console.log("targeted CEF instance was destroyed");
                            }

                            //exception was thrown, patching thread crashed, so mark it as unpatched so it can restart itself, happens when the 
                            //instance is left alone for too long sometimes and steams garbage collector clears memory
                            std::vector<std::string>::iterator it = std::remove_if(patched.begin(), patched.end(), [&](const std::string& element) {
                                return element == instance["webSocketDebuggerUrl"];
                            });

                            patched.erase(it, patched.end());
                        }
                    });
                }
            }
            // Detach all the threads and let them run concurrently
            for (auto& thread : threads) {
                thread.detach();
            }
        });
    }
};

/// <summary>
/// start the steam IPC to communicate between C++ and js
/// </summary>
void steam_to_millennium_ipc() 
{
    try {
        boost::asio::io_context ioc;
        millennium_ipc_listener listener(ioc);
        ioc.run();
    }
    catch (std::exception& e) {
        console.log("ipc exception " + std::string(e.what()));
    }
}

/// <summary>
/// initialize millennium components
/// </summary>
steam_client::steam_client()
{
    std::thread steam_client_interface_handler_threadworker([this]() {
        try {
            client::get_instance().handle_interface();
        }
        catch (nlohmann::json::exception& ex) { console.err("client: " + std::string(ex.what()) + "\nline: " + std::to_string(__LINE__)); }
        catch (const boost::system::system_error& ex) { console.err("client: " + std::string(ex.what()) + "\nline: " + std::to_string(__LINE__)); }
        catch (const std::exception& ex) { console.err("client: " + std::string(ex.what()) + "\nline: " + std::to_string(__LINE__)); }
        catch (...) { console.err("client: unkown"); }

    });
    std::thread steam_remote_interface_handler_threadworker([this]() {
        try {
            remote::get_instance().handle_interface();
        }
        catch (nlohmann::json::exception& ex) { console.err("remote: " + std::string(ex.what()) + "\nline: " + std::to_string(__LINE__)); }
        catch (const boost::system::system_error& ex) { console.err("remote: " + std::string(ex.what()) + "\nline: " + std::to_string(__LINE__)); }
        catch (const std::exception& ex) { console.err("remote: " + std::string(ex.what()) + "\nline: " + std::to_string(__LINE__)); }
        catch (...) { console.err("remote: unknown"); }
    });
    std::thread steam_to_millennium_ipc_threadworker([this]() { steam_to_millennium_ipc(); });

    //worker threads to run all code at the same time
    steam_client_interface_handler_threadworker.join();
    steam_remote_interface_handler_threadworker.join();
    steam_to_millennium_ipc_threadworker.join();
}

/// <summary>
/// yet another initializer
/// </summary>
/// <param name=""></param>
/// <returns></returns>
unsigned long __stdcall Initialize(void*)
{
    config.verify_registry();
    skin_json_config = config.get_skin_config();

    //skin change event callback functions
    skin_config::skin_change_events::get_instance().add_listener([]() {
        console.log("skin change event fired, updating skin patch config");
        skin_json_config = config.get_skin_config();
    });

    //config file watcher callback function 
    std::thread watcher(skin_config::watch_config, std::format("{}/{}/skin.json", config.get_steam_skin_path(), registry::get_registry("active-skin")), []() {
        console.log("skin configuration changed, updating");
        skin_json_config = config.get_skin_config();
    });

    //create steamclient object
    steam_client ISteamHandler;
    watcher.join();
    return true;
}