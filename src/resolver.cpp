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
#include <cctype> // input validation functions

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

        std::cout << "\n\t" 
                  << m_hostname << ":" << "\n\t"
                  << std::string(m_hostname.size()+1 , '-')
                  << std::endl;

        std::for_each(it, it_end, [this, &index](asio::ip::tcp::endpoint ep) {
            std::cout << "\tEndpoint " << index << ": "
                      << ep.address() << " "
                      << (ep.address().is_v4() ? "(IPv4)" : "(IPv6)")
                      << '\n';
            
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
                    std::string hostname_input = get_hostname();
                    m_resolver.set_hostname(hostname_input);
                    break;
                }
                case SET_PORT: {
                    unsigned short port_input = static_cast<unsigned short>(get_port_number());
                    m_resolver.set_port_number(port_input);
                    break;
                }
                case RESOLVE_DNS: {
                    // Resolver::resolve_dns will start an asynchronous resolve operation
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
        std::cout << "\n\t0 - Exit\n"
            "\t1 - Set hostname\n"
            "\t2 - Set port number\n"
            "\t3 - Resolver DNS\n"
            "\t9 - Show commands\n\n";
    }

    // Gets a hostname and performs some input validation
    std::string get_hostname() 
    {
        const unsigned int MINIMUM_CHARACTER_COUNT = 3;

        std::string hostname;
        std::cin.clear();   // clear state flags, set goodbit
        std::cin.ignore();  // remove newline character if present

        do {
            // Get hostname from user
            std::cout << "Enter a new hostname: ";
            std::getline(std::cin, hostname, '\n');

            // Check if hostname string is empty
            if (hostname.empty()) {
                std::cout << "\t> hostname cannot be empty.\n";
                continue;
            }

            // Input validation
            bool rejected = false;
            unsigned short period_count = 0;
            unsigned int character_count = 0;

            for (std::size_t i = 0; i < hostname.length(); ++i) 
            {
                // Continue if character is a letter or number
                if (std::isalpha(hostname[i]) || std::isdigit(hostname[i])) {
                    ++character_count;
                    continue;
                }
                else if (hostname[i] == '.') {
                    ++period_count;
                    continue;
                }
                else {
                    // Hostname contains a non-alphanumeric character
                    rejected = true;
                    std::cout << "\t> hostname must contain only periods and alphanumeric characters.\n";
                    break;
                }
            }

            // Verify that the hostname as more than 3 characters
            if (character_count < MINIMUM_CHARACTER_COUNT) {
                rejected = true;
                std::cout << "\t> hostname must contain more than " << MINIMUM_CHARACTER_COUNT  
                    << " characters.\n";
            }

            // Verify number of periods (Hostnames are allowed more than 1 period)
            if (period_count == 0) {
                rejected = true;
                std::cout << "\t> hostname must contain a period (.) character.\n";
            }

            if (!rejected)
                break;
        } while (true);

        std::cout << "\t> Hostname set to: " << hostname << "\n\n";
        return hostname;
    }

    // Gets a port number and performs some input validation
    short get_port_number()
    {
        std::cin.clear();   // clear state flags and set goodbit
        std::cin.ignore();  // remove newline character if present

        short port_number;

        do {
            std::cout << "Enter a new port number: ";
            std::cin >> port_number;

            // If failbit is set, no extraction took place
            if (std::cin.fail()) 
            {    
                // Clear state bits and set goodbit (cannot use cin if failbit is set)
                std::cin.clear();

                // Remove bad input from stream (ignores up to whitespace)
                std::cin.ignore(INT_MAX, '\n');
                
                std::cout << "\t> Invalid port number: Could not extract an integer.\n";
                continue;
            }
            
            // Check if extra data is still in the stream after reading an initial number.
            std::cin.ignore(INT_MAX, '\n'); // clear out any additional input from stream
            
            // If stream ignored more than 1 character, consider this invalid
            if (std::cin.gcount() > 1) {
                std::cout << "\t> Invalid port number: Extra data found in stream ("
                    << std::cin.gcount() << " extra characters).\n";
                continue;
            }

            // Check that port number is a positive integer
            if (port_number <= 0) {
                std::cout << "\t> Invalid port number: Value must be greater than zero.\n";
                continue;
            }

            break;

        } while (true);

        std::cout << "\t> Port number set to: " << port_number << "\n\n";

        return port_number;
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