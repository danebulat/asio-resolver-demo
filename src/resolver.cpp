/*
Boost.Asio application that resolves a DNS hostname to its public IP addresses.
Uses a mutex and condition variable to synchronize the "UI thread" and "I/O thread".
Threads:
    - UI thread:         Handles user input.
    - I/O thread:        Resolves a DNS name asynchronously.
    - io_service thread: Runs the asio::io_service loop.
*/

#include <boost/asio.hpp>

#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>

using namespace boost;

// ----------------------------------------------------------------------
// Resolver
// ----------------------------------------------------------------------

class Resolver {
public:
    // Constructors
    Resolver(asio::io_service& ios) : m_ios(ios), m_resolver(ios), m_resolved(false) {
        initialise_io();
    }

    Resolver(asio::io_service& ios, std::string& hostname, unsigned short port_number)
        : m_ios(ios), 
          m_hostname(hostname),
          m_port_number(std::to_string(port_number)),
          m_resolver(m_ios),
          m_resolved(false)
    {
        initialise_io();
    }

    // Destructor
    ~Resolver() {
        std::cout << "Resolver::Destructor" << std::endl;
    }

    void close() {
        m_work.reset(nullptr);
        m_iothread->join();
    }

    bool resolve_dns() {
        try {
            // Verify hostname and port number are not empty
            if (!verify()) { 
                throw std::string("Bad hostname or port number strings.");
            }

            // Create resolver query
            asio::ip::tcp::resolver::query resolver_query(m_hostname,
                m_port_number, asio::ip::tcp::resolver::query::numeric_service);
            
            // Asynchronously resolve DNS name to endpoints
            m_resolver.async_resolve(resolver_query, 
                [this]
                (const system::error_code& ec, asio::ip::tcp::resolver::iterator it) {
                    
                    // Callback to process endpoints
                    on_process_endpoints(ec, it);
                });
            
            return true;
        }
        catch (std::string& error) {
            std::cerr << "Error: " << error << std::endl;
            //return false;
        }

        return false;
    }

    void set_condition_variable(std::shared_ptr<std::mutex> mutex, std::shared_ptr<std::condition_variable> condition) {
        m_mutex = mutex;
        m_condition = condition;
    }

    void set_hostname(const std::string& hostname) {
        m_hostname = hostname;
    }

    void set_port_number(unsigned short port_number) {
        m_port_number = std::to_string(port_number);
    }

    void set_resolved(bool resolved) {
        m_resolved = resolved;
    }

    bool get_resolved() {
        return m_resolved;
    }

private:
    void initialise_io() 
    {
        // Get I/O service work object
        m_work.reset(new asio::io_service::work(m_ios));

        // Run I/O loop in separate thread
        m_iothread.reset(new std::thread([this](){
            m_ios.run();
        }));
    }

    void on_process_endpoints(const system::error_code& ec, 
        asio::ip::tcp::resolver::iterator& it) 
    {
        // Handle any errors
        if (ec.value() != 0) {
            std::cerr << "Error resolving query." << std::endl
                  << "Error code: " << ec.value() << std::endl
                  << "Error message: " << ec.message() << std::endl;
        }

        // Output endpoints to console
        output_endpoints(it);
    }
    
    bool verify() {
        if (m_hostname.empty() || m_port_number.empty())
            return false;
        
        return true;
    }

    void output_endpoints(asio::ip::tcp::resolver::iterator& it) 
    {
        // NOTE: Mutex needed to use `cout` if condition variable was not used
        asio::ip::tcp::resolver::iterator it_end;
        unsigned int index = 0;

        std::cout << std::endl 
                  << m_hostname << ":" << std::endl
                  << std::string(m_hostname.size()+1 , '-')
                  << std::endl;

        std::for_each(it, it_end, [this, &index](asio::ip::tcp::endpoint ep) {
            std::cout << "Endpoint " << index << ": "
                      << ep.address() << " "
                      << (ep.address().is_v4() ? "(IPv4)" : "(IPv6)")
                      << std::endl;
            
            ++index;
        });

        std::cout << std::endl;

        // Notify UI thread waiting for resolving to finish
        std::unique_lock<std::mutex> lock(*m_mutex.get());
        set_resolved(true);

        m_condition->notify_one();
        lock.unlock();
    }

private:
    asio::io_service& m_ios;
    std::unique_ptr<asio::io_service::work> m_work;
    std::unique_ptr<std::thread> m_iothread;

    asio::ip::tcp::resolver m_resolver;
    std::string m_hostname;
    std::string m_port_number;

    bool m_resolved;
    std::shared_ptr<std::mutex> m_mutex;
    std::shared_ptr<std::condition_variable> m_condition;
};

// ----------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------
class InputManager {
    enum Commands {
        EXIT = 0,
        SET_HOSTNAME,
        SET_PORT,
        RESOLVE_DNS,
        DISPLAY_COMMANDS = 9
    };

public:
    InputManager(Resolver& resolver) : m_resolver(resolver), m_exit(false)
    {
        // Allocate mutex and condition variable and send to resolver object.
        m_mutex.reset(new std::mutex);
        m_condition.reset(new std::condition_variable);
        m_resolver.set_condition_variable(m_mutex, m_condition);
    }

    ~InputManager() {
        std::cout << "InputManager::Destructor" << std::endl;
    }

    void Run() 
    {
        // Display command options
        display_commands();

        while (!m_exit) 
        {
            // Get input from user and process command
            int input = get_input();

            switch (input)
            {
                case EXIT: {
                    m_resolver.close();
                    m_exit = true;
                    break;
                }
                case SET_HOSTNAME: {
                    std::string hostname_input;
                    std::cout << "> Enter new hostname: ";
                    std::cin >> hostname_input;
                    std::cout << "> Hostname set to: " << hostname_input << "\n\n";
                    m_resolver.set_hostname(hostname_input);
                    break;
                }
                case SET_PORT: {
                    unsigned short port_input = 0;
                    std::cout << "> Enter new port number: ";
                    std::cin >> port_input;
                    std::cout << "> Port number set to: " << port_input << "\n\n";
                    m_resolver.set_port_number(port_input);
                    break;
                }
                case RESOLVE_DNS: {
                    if (m_resolver.resolve_dns())
                    {
                        // Wait until the I/O thread has output results to console with `cout`
                        std::unique_lock<std::mutex> lock(*m_mutex.get());
                        m_condition->wait(lock, std::bind(&Resolver::get_resolved, &m_resolver));

                        m_resolver.set_resolved(false);
                        lock.unlock();
                        
                        break;
                    }
                }
                case DISPLAY_COMMANDS: {
                    display_commands();
                    break;
                }
                default: {
                    std::cout << "> Command unrecognised..." << std::endl;
                    break;
                }
            } // end switch
        }
    }

private:
    int get_input() {
        int input;
        std::cout << "> Enter command: ";
        std::cin >> input;

        return input;
    }

    void display_commands() {
        std::cout << "\n0 - Exit\n"
            "1 - Set hostname\n"
            "2 - Set port number\n"
            "3 - Resolver DNS\n"
            "9 - Show commands\n\n";
    }

private:
    Resolver& m_resolver;
    bool m_exit;

    std::shared_ptr<std::condition_variable> m_condition;
    std::shared_ptr<std::mutex> m_mutex;
};

// ----------------------------------------------------------------------
// main
// ----------------------------------------------------------------------

int main(int argc, char* argv[])
{
    asio::io_service ios;

    Resolver resolver(ios);
    InputManager input_manager(resolver);

    input_manager.Run();

    return 0;
}