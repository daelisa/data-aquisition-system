#include <boost/asio.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <memory>
#include <mutex>

#pragma pack(push, 1)
struct LogRecord {
    char sensor_id[32];
    std::time_t timestamp;
    double value;
};
#pragma pack(pop)

std::mutex file_mutex; // Para proteger acesso a arquivos

std::time_t string_to_time_t(const std::string& time_string) {
    std::tm tm = {};
    std::istringstream ss(time_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::mktime(&tm);
}

std::string time_t_to_string(std::time_t time) {
    std::tm* tm = std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

void log_sensor_data(const std::string& sensor_id, std::time_t timestamp, double value) {
    LogRecord record{};
    strncpy(record.sensor_id, sensor_id.c_str(), sizeof(record.sensor_id) - 1);
    record.timestamp = timestamp;
    record.value = value;

    std::lock_guard<std::mutex> lock(file_mutex); // Evita acesso simultâneo ao arquivo
    std::ofstream file(sensor_id + ".log", std::ios::binary | std::ios::app);
    file.write(reinterpret_cast<const char*>(&record), sizeof(record));
}

std::vector<LogRecord> get_last_records(const std::string& sensor_id, int num_records) {
    std::vector<LogRecord> records;

    std::lock_guard<std::mutex> lock(file_mutex);
    std::ifstream file(sensor_id + ".log", std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Invalid sensor ID");
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    size_t record_size = sizeof(LogRecord);
    size_t num_available_records = file_size / record_size;

    int records_to_read = std::min(num_records, static_cast<int>(num_available_records));
    file.seekg(-records_to_read * record_size, std::ios::end);

    for (int i = 0; i < records_to_read; ++i) {
        LogRecord record;
        file.read(reinterpret_cast<char*>(&record), record_size);
        records.push_back(record);
    }

    return records;
}

class SensorServer {
public:
    SensorServer(boost::asio::io_context& io_context, unsigned short port)
        : acceptor_(io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)) {
        start_accept();
    }

private:
    void start_accept() {
        auto socket = std::make_shared<boost::asio::ip::tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& error) {
            if (!error) {
                handle_connection(socket);
            }
            start_accept();
        });
    }

    void handle_connection(std::shared_ptr<boost::asio::ip::tcp::socket> socket) {
        auto buffer = std::make_shared<std::array<char, 1024>>();
        socket->async_read_some(boost::asio::buffer(*buffer), [this, socket, buffer](const boost::system::error_code& error, std::size_t bytes_transferred) {
            if (!error) {
                std::string message(buffer->data(), bytes_transferred);
                process_message(socket, message);
            }
        });
    }

    void process_message(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const std::string& message) {
        if (message.rfind("LOG|", 0) == 0) {
            handle_log_message(message);
        } else if (message.rfind("GET|", 0) == 0) {
            handle_get_message(socket, message);
        } else {
            send_response(socket, "ERROR|INVALID_COMMAND\r\n");
        }
    }

    void handle_log_message(const std::string& message) {
        auto parts = split_message(message, '|');
        if (parts.size() != 4) return;

        std::string sensor_id = parts[1];
        std::time_t timestamp = string_to_time_t(parts[2]);
        double value = std::stod(parts[3]);

        log_sensor_data(sensor_id, timestamp, value);
    }

    void handle_get_message(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const std::string& message) {
        auto parts = split_message(message, '|');
        if (parts.size() != 3) return;

        std::string sensor_id = parts[1];
        int num_records = std::stoi(parts[2]);

        try {
            auto records = get_last_records(sensor_id, num_records);
            std::string response = std::to_string(records.size()) + ";";
            for (const auto& record : records) {
                response += time_t_to_string(record.timestamp) + "|" + std::to_string(record.value) + ";";
            }
            send_response(socket, response + "\r\n");
        } catch (const std::runtime_error&) {
            send_response(socket, "ERROR|INVALID_SENSOR_ID\r\n");
        }
    }

    void send_response(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const std::string& response) {
        auto buffer = std::make_shared<std::string>(response);
        boost::asio::async_write(*socket, boost::asio::buffer(*buffer), [buffer](const boost::system::error_code&, std::size_t) {
            // Resposta enviada
        });
    }

    std::vector<std::string> split_message(const std::string& message, char delimiter) {
        std::vector<std::string> parts;
        std::istringstream stream(message);
        std::string part;
        while (std::getline(stream, part, delimiter)) {
            parts.push_back(part);
        }
        return parts;
    }

    boost::asio::ip::tcp::acceptor acceptor_;
};
