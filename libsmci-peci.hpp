#pragma once
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <fstream>
#include <json.hpp>
#include <systemd/sd-journal.h>
#include "peci.h"

using json = nlohmann::json;

class PeciExecutor {
public:
    explicit PeciExecutor(const std::string& json_file_path, uint8_t max_cpu = 8): m_max_cpu(max_cpu) {
        std::ifstream json_file(json_file_path);
        if (!json_file.is_open()) {
            sd_journal_print(LOG_ERR, "Error opening JSON file\n");
            throw std::runtime_error("Error opening JSON file");
        }
        json_file >> m_input_json;
    }

    void executeCommands() {
        detectOnlineCPUs();

        json commandsArray = json::array();

        for (const auto& [command_name, command_list] : m_input_json["commands"].items()) {
            for (auto& command : command_list) {
                for (const auto& target : m_online_cpus) {
                    json updated_params = substituteTargetParameter(command["Params"], target);
                    json res;
                    executeCommand(command_name, updated_params, res);

                    json outputCmd;
                    outputCmd["name"] = command_name;
                    outputCmd["target"] = hexToString(target);
                    outputCmd["params"] = updated_params;
                    outputCmd["res"] = res;

                    commandsArray.push_back(outputCmd);
                }
            }
        }
        m_output_json["commands"] = commandsArray;
    }

    void saveOutputJsonToFile(const std::string &output_json_file) {
        std::ofstream output_file(output_json_file);
        output_file << m_output_json.dump(4);
        output_file.close();
    }


private:
    json m_input_json;
    json m_output_json;
    std::vector<uint8_t> m_online_cpus;
    uint8_t m_max_cpu;

    void detectOnlineCPUs() {
        for (uint8_t target = 0x30; target < (0x30 + m_max_cpu); ++target) {
            EPECIStatus status = peci_Ping(target);
            if (status == EPECIStatus::PECI_CC_SUCCESS) {
                m_online_cpus.push_back(target);
                sd_journal_print(LOG_INFO, "peci_Ping found target: 0x%02x\n", target);
            }
        }
    }

    json substituteTargetParameter(const json& params, uint8_t target) {
        json updated_params = params;
        for (auto& param : updated_params) {
            if (param.is_string() && param.get<std::string>() == "Target") {
                param = target;
            }
        }
        return updated_params;
    }

    void executeCommand(const std::string& command_name, const json& params, json& res) {
        if (command_name == "GetCPUID") {
            uint8_t Target = params[0];
            
            CPUModel cpuModel;
            uint8_t stepping = 0;
            uint8_t cc = 0;

            EPECIStatus status = peci_GetCPUID(Target, &cpuModel, &stepping, &cc);
            if (status != EPECIStatus::PECI_CC_SUCCESS) {
                sd_journal_print(LOG_ERR, "%s failed with status: %d, cc: %s\n",
                    command_name.c_str(), status, hexToString(cc).c_str());
            } else {
                res["model"] = hexToString(cpuModel);
                res["stepping"] = hexToString(stepping);
                res["c"] = hexToString(cc);
            }
        } else if (command_name == "GetTemp") {
            uint8_t Target = params[0];
            
            int16_t rawTemperature;

            EPECIStatus status = peci_GetTemp(Target, &rawTemperature);
            if (status != EPECIStatus::PECI_CC_SUCCESS) {
                sd_journal_print(LOG_ERR, "%s failed with status: %d\n",
                    command_name.c_str(), status);
            } else {
                float temperatureInCelsius = static_cast<float>(rawTemperature) / 64.0f;
                res["rel_temp"] = temperatureInCelsius;
            }
        } else if (command_name == "RdPkgConfig") {
            uint8_t Target = params[0];
            uint8_t Index = params[1];
            uint16_t Value = params[2];
            uint8_t ReadLen = params[3];

            uint8_t pPkgConfig[255];
            uint8_t cc;

            EPECIStatus status = peci_RdPkgConfig(Target, Index, Value, ReadLen, pPkgConfig, &cc);
            if (status != EPECIStatus::PECI_CC_SUCCESS) {
                sd_journal_print(LOG_ERR, "%s failed with status: %d, cc: %s\n",
                    command_name.c_str(), status, hexToString(cc).c_str());
            } else {
                res["p"] = bytesToHexJsonArray(std::vector<uint8_t>(pPkgConfig, pPkgConfig + ReadLen));
                res["c"] = hexToString(cc);
            }
        } else if (command_name == "RdIAMSR") {
            uint8_t Target = params[0];
            uint8_t threadID = params[1];
            uint16_t MSRAddress = std::stoi(params[2].get<std::string>(), nullptr, 16);

            uint64_t u64MsrVal;
            uint8_t cc;

            EPECIStatus status = peci_RdIAMSR(Target, threadID, MSRAddress, &u64MsrVal, &cc);
            if (status != EPECIStatus::PECI_CC_SUCCESS) {
                sd_journal_print(LOG_ERR, "%s failed with status: %d, cc: %s\n",
                    command_name.c_str(), status, hexToString(cc).c_str());
            } else {
                res["p"] = hexToString(u64MsrVal);
                res["c"] = hexToString(cc);
            }
        } else if (command_name == "Telemetry_Discovery") {
            uint8_t target = params[0];
            uint8_t subopcode = params[1];
            uint8_t param0 = params[2];
            uint16_t param1 = std::stoi(params[3].get<std::string>(), nullptr, 16);
            uint8_t param2 = params[4];
            uint8_t ReadLen = params[5];

            uint8_t data[ReadLen];
            uint8_t cc;

            EPECIStatus status = peci_Telemetry_Discovery(target, subopcode, param0, param1, param2, ReadLen, data, &cc);
            if (status != EPECIStatus::PECI_CC_SUCCESS) {
                sd_journal_print(LOG_ERR, "%s failed with status: %d, cc: %s\n",
                    command_name.c_str(), status, hexToString(cc).c_str());
            } else {
                res["p"] = bytesToHexJsonArray(std::vector<uint8_t>(data, data + ReadLen));
                res["c"] = hexToString(cc);
            }
        } else {
            sd_journal_print(LOG_ERR, "unknown command: %s\n", command_name.c_str());
        }
    }

    std::string hexToString(uint64_t value, bool add_prefix = true) {
        std::stringstream ss;
        if (add_prefix) {
            ss << "0x";
        }
        ss << std::setfill('0') << std::setw(2) << std::hex << value;
        return ss.str();
    }

    std::string bytesToHexString(const std::vector<uint8_t>& data) {
        std::string hex_string;
        hex_string.reserve(2 * data.size() + 2);
        hex_string = "0x";

        for (const auto& byte : data) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", byte);
            hex_string.append(buf);
        }
        return hex_string;
    }

    json bytesToHexJsonArray(const std::vector<uint8_t>& data) {
        json hex_array = json::array();

        for (const auto& byte : data) {
            std::stringstream hex_stream;
            hex_stream << "0x" << std::setfill('0') << std::setw(2) << std::hex << static_cast<int>(byte);
            hex_array.push_back(hex_stream.str());
        }

        return hex_array;
    }
};
