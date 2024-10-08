#include "vACDM.h"

#include <Windows.h>
#include <shlwapi.h>

#include <numeric>

#include "Version.h"
#include "config/ConfigParser.h"
#include "core/CompileCommands.h"
#include "core/DataManager.h"
#include "core/Server.h"
#include "core/TagFunctions.h"
#include "core/TagItems.h"
#include "log/Logger.h"
#include "utils/Date.h"
#include "utils/Number.h"
#include "utils/String.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

using namespace vacdm;
using namespace vacdm::com;
using namespace vacdm::core;
using namespace vacdm::logging;
using namespace vacdm::utils;

namespace vacdm {
vACDM::vACDM()
    : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, PLUGIN_NAME, PLUGIN_VERSION, PLUGIN_AUTHOR, PLUGIN_LICENSE) {
    DisplayMessage("Version " + std::string(PLUGIN_VERSION) + " loaded", "Initialisation");
    Logger::instance().log(Logger::LogSender::vACDM, "Version " + std::string(PLUGIN_VERSION) + " loaded",
                           Logger::LogLevel::System);

    if (0 != curl_global_init(CURL_GLOBAL_ALL)) DisplayMessage("Unable to initialize the network stack!");

    // get the dll-path
    char path[MAX_PATH + 1] = {0};
    GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
    PathRemoveFileSpecA(path);
    this->m_dllPath = std::string(path);

    this->RegisterTagItemTypes();
    this->RegisterTagItemFuntions();

    this->reloadConfiguration(true);
}

vACDM::~vACDM() {}

void vACDM::DisplayMessage(const std::string &message, const std::string &sender) {
    DisplayUserMessage("vACDM", sender.c_str(), message.c_str(), true, false, false, false, false);
}

void vACDM::checkServerConfiguration() {
    if (Server::instance().checkWebApi() == false) {
        DisplayMessage("Connection failed.", "Server");
        DisplayMessage(Server::instance().errorMessage().c_str(), "Server");
    } else {
        std::string serverName = Server::instance().getServerConfig().name;
        DisplayMessage(("Connected to " + serverName), "Server");
        // set active airports and runways
        this->OnAirportRunwayActivityChanged();
    }
}

void vACDM::runEuroscopeUpdate() {
    for (EuroScopePlugIn::CFlightPlan flightplan = FlightPlanSelectFirst(); flightplan.IsValid();
         flightplan = FlightPlanSelectNext(flightplan)) {
        DataManager::instance().queueFlightplanUpdate(flightplan);
    }
}

void vACDM::SetGroundState(const EuroScopePlugIn::CFlightPlan flightplan, const std::string groundstate) {
    // using GRP and default Euroscope ground states
    // STATE                    ABBREVIATION    GRP STATE
    // - No state(departure)    NSTS
    // - On Freq                ONFREQ              Y
    // - De - Ice               DE-ICE              Y
    // - Start - Up             STUP
    // - Pushback               PUSH
    // - Taxi                   TAXI
    // - Line Up                LINEUP              Y
    // - Taxi In                TXIN
    // - No state(arrival)      NOSTATE             Y
    // - Parked                 PARK

    std::string scratchBackup(flightplan.GetControllerAssignedData().GetScratchPadString());
    flightplan.GetControllerAssignedData().SetScratchPadString(groundstate.c_str());
    flightplan.GetControllerAssignedData().SetScratchPadString(scratchBackup.c_str());
}

void vACDM::reloadConfiguration(bool initialLoading) {
    PluginConfig newConfig;
    ConfigParser parser;

    if (false == parser.parse(this->m_dllPath + this->m_configFileName, newConfig) || false == newConfig.valid) {
        std::string message = "vacdm.txt:" + std::to_string(parser.errorLine()) + ": " + parser.errorMessage();
        DisplayMessage(message, "Config");
    } else {
        DisplayMessage(true == initialLoading ? "Loaded the config" : "Reloaded the config", "Config");
        if (this->m_pluginConfig.serverUrl != newConfig.serverUrl)
            this->changeServerUrl(newConfig.serverUrl);
        else
            this->checkServerConfiguration();

        this->m_pluginConfig = newConfig;
        DisplayMessage(DataManager::instance().setUpdateCycleSeconds(newConfig.updateCycleSeconds));
        tagitems::Color::updatePluginConfig(newConfig);
    }
}

void vACDM::changeServerUrl(const std::string &url) {
    DataManager::instance().pause();
    Server::instance().changeServerAddress(url);
    this->checkServerConfiguration();

    DataManager::instance().resume();
    DisplayMessage("Changed URL to " + url);
    Logger::instance().log(Logger::LogSender::vACDM, "Changed URL to " + url, Logger::LogLevel::Info);
}

// Euroscope Events:

void vACDM::OnTimer(int Counter) {
    if (Counter % 5 == 0) this->runEuroscopeUpdate();
}

void vACDM::OnFlightPlanFlightPlanDataUpdate(EuroScopePlugIn::CFlightPlan FlightPlan) {
    DataManager::instance().queueFlightplanUpdate(FlightPlan);
}

void vACDM::OnFlightPlanControllerAssignedDataUpdate(EuroScopePlugIn::CFlightPlan FlightPlan, int DataType) {
    // preemptive return to only handle relevant changes
    if (EuroScopePlugIn::CTR_DATA_TYPE_SPEED == DataType || EuroScopePlugIn::CTR_DATA_TYPE_MACH == DataType ||
        EuroScopePlugIn::CTR_DATA_TYPE_RATE == DataType || EuroScopePlugIn::CTR_DATA_TYPE_HEADING == DataType ||
        EuroScopePlugIn::CTR_DATA_TYPE_DIRECT_TO == DataType) {
        return;
    }
    DataManager::instance().queueFlightplanUpdate(FlightPlan);
}

void vACDM::OnAirportRunwayActivityChanged() {
    std::list<std::string> activeAirports;

    EuroScopePlugIn::CSectorElement airport;
    for (airport = this->SectorFileElementSelectFirst(EuroScopePlugIn::SECTOR_ELEMENT_AIRPORT);
         airport.IsValid() == true;
         airport = this->SectorFileElementSelectNext(airport, EuroScopePlugIn::SECTOR_ELEMENT_AIRPORT)) {
        // skip airport if it is selected as active airport for departures or arrivals
        if (false == airport.IsElementActive(true, 0) && false == airport.IsElementActive(false, 0)) continue;

        // get the airport ICAO
        auto airportICAO = utils::String::findIcao(utils::String::trim(airport.GetName()));
        // skip airport if no ICAO has been found
        if (airportICAO == "") continue;

        // check if the airport has been added already, add if it does not exist
        if (std::find(activeAirports.begin(), activeAirports.end(), airportICAO) == activeAirports.end()) {
            activeAirports.push_back(airportICAO);
        }
    }

    if (activeAirports.empty()) {
        Logger::instance().log(Logger::LogSender::vACDM,
                               "Airport/Runway Change, no active airports: ", Logger::LogLevel::Info);
    } else {
        Logger::instance().log(
            Logger::LogSender::vACDM,
            "Airport/Runway Change, active airports: " +
                std::accumulate(std::next(activeAirports.begin()), activeAirports.end(), activeAirports.front(),
                                [](const std::string &acc, const std::string &str) { return acc + " " + str; }),
            Logger::LogLevel::Info);
    }
    DataManager::instance().setActiveAirports(activeAirports);
}

}  // namespace vacdm