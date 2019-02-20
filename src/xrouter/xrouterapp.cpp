//*****************************************************************************
//*****************************************************************************

#include "xrouterapp.h"
#include "init.h"
#include "keystore.h"
#include "main.h"
#include "net.h"
#include "servicenodeconfig.h"
#include "servicenodeman.h"
#include "addrman.h"
#include "script/standard.h"
#include "wallet.h"
#include "bloom.h"
#include "rpcserver.h"

#include "xbridge/util/settings.h"
#include "xbridge/bitcoinrpcconnector.h"
#include "xrouterlogger.h"

#include "xrouterutils.h"
#include "xroutererror.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <assert.h>

#include <boost/chrono/chrono.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/map.hpp>
#include <iostream>
#include <sstream>
#include <vector>
#include <chrono>

//*****************************************************************************
//*****************************************************************************
namespace xrouter
{   
boost::container::map<CNode*, double > App::snodeScore = boost::container::map<CNode*, double >(); 
typedef boost::container::map<CNode*, std::string> queries_map;

//*****************************************************************************
//*****************************************************************************
App::App()
    : server(new XRouterServer), queries()
{
}

//*****************************************************************************
//*****************************************************************************
App::~App()
{
    stop();
}

//*****************************************************************************
//*****************************************************************************
// static
App& App::instance()
{
    static App app;
    return app;
}

//*****************************************************************************
//*****************************************************************************
// static
bool App::isEnabled()
{
    // enabled by default
    return GetBoolArg("-xrouter", false);
}

bool App::init(int argc, char *argv[])
{
    if (!isEnabled())
        return true;
    std::string path(GetDataDir(false).string());
    this->xrouterpath = path + "/xrouter.conf";
    LOG() << "Loading xrouter config from file " << xrouterpath;
    this->xrouter_settings.read(xrouterpath.c_str());
    this->xrouter_settings.loadPlugins();

    // init xbridge settings
    Settings & s = settings();
    
    std::string xbridgepath = path + "/xbridge.conf";
    s.read(xbridgepath.c_str());
    s.parseCmdLine(argc, argv);
    LOG() << "Loading xbridge config from file " << xbridgepath;
    
    this->loadPaymentChannels();
    
    return true;
}

std::vector<std::string> App::getServicesList() 
{
    // We append "XRouter" if XRouter is activated at all, and "XRouter::service_name" for each activated plugin
    std::vector<std::string> result;
    if (!isEnabled())
        return result;
    result.push_back("XRouter");
    LOG() << "Adding XRouter to servicenode ping";
    for (std::string s : xrouter_settings.getPlugins()) {
        result.push_back("XRouter::" + s);
        LOG() << "Adding XRouter plugin " << s << " to servicenode ping";
    }
    return result;
}

static std::vector<pair<int, CServicenode> > getServiceNodes()
{
    int nHeight;
    {
        LOCK(cs_main);
        CBlockIndex* pindex = chainActive.Tip();
        if(!pindex) return std::vector<pair<int, CServicenode> >();
        nHeight = pindex->nHeight;
    }
    return mnodeman.GetServicenodeRanks(nHeight);
}

//*****************************************************************************
//*****************************************************************************
bool App::start()
{
    if (!isEnabled())
        return true;
    
    updateConfigs();
    bool res = server->start();
    //openConnections();
    return res;
}

void App::openConnections(std::string wallet, std::string plugin)
{
    // Open connections to all service nodes that are not already our peers, but have XRouter functionality accroding to serviceping
    LOG() << "Current peers count = " << vNodes.size();
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();
    BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {
        if (!s.second.HasService("XRouter"))
            continue;
        
        if (wallet != "")
            if (!s.second.HasService(wallet))
                continue;
        
        if (plugin != "")
            if (!s.second.HasService("XRouter::" + plugin))
                continue;
        
        bool connected = false;
        for (CNode* pnode : vNodes) {
            if (s.second.addr.ToString() == pnode->addr.ToString()) {
                connected = true;
            }
        }
        
        if (!connected) {
            CAddress addr;
            CNode* res = ConnectNode(addr, s.second.addr.ToString().c_str());
            LOG() << "Trying to connect to " << CBitcoinAddress(s.second.pubKeyCollateralAddress.GetID()).ToString() << "; result=" << ((res == NULL) ? "fail" : "success");
            if (res) {
                this->getXrouterConfig(res);
                XRouterPeer peer;
                peer.node = res;
                peer.serviceNode = s.second;
                this->peers[res->addr.ToString()] = peer;
            }
        }
    }

    LOG() << "Current peers count = " << vNodes.size();
}

std::string App::updateConfigs()
{
    if (!isEnabled())
        return "XRouter is turned off. Please set 'xrouter=1' in blocknetdx.conf to run XRouter.";
    
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();
    std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
    
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (snodeConfigs.count(pnode->addr.ToString())) {
            continue;
        }

        if (lastConfigUpdates.count(pnode)) {
            // There was a request to this node already, a new one will be sent only after 5 minutes
            std::chrono::time_point<std::chrono::system_clock> prev_time = lastConfigUpdates[pnode];
            std::chrono::system_clock::duration diff = time - prev_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(diff) < std::chrono::seconds(300)) 
                continue;
        }
         
        if (debug_on_client()) {
            std::string uuid = this->getXrouterConfig(pnode);
            LOG() << "Getting config from node " << pnode->addr.ToString()  << " request id = " << uuid;
            lastConfigUpdates[pnode] = time;
            continue;
        }
        
        BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {
            if (s.second.addr.ToString() == pnode->addr.ToString()) {
                // This node is a service node
                std::string uuid = this->getXrouterConfig(pnode);
                LOG() << "Getting config from node " << CBitcoinAddress(s.second.pubKeyCollateralAddress.GetID()).ToString() << " request id = " << uuid;
                lastConfigUpdates[pnode] = time;
            }
        }
    }
    
    BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {
        bool found = false;
        for (CNode* pnode : vNodes) {
            if (s.second.addr.ToString() == pnode->addr.ToString()) {
                found = true;
                break;
            }
        }
        
        // TODO: this code needs revision
        break;
        
        if (!found) {
            LOG() << "Broadcasting request for config of snode " << s.second.addr.ToString();
            for (CNode* pnode : vNodes) {
                if (lastConfigUpdates.count(pnode)) {
                    // There was a request to this node already, a new one will be sent only after 5 minutes
                    std::chrono::time_point<std::chrono::system_clock> prev_time = lastConfigUpdates[pnode];
                    std::chrono::system_clock::duration diff = time - prev_time;
                    if (std::chrono::duration_cast<std::chrono::seconds>(diff) < std::chrono::seconds(300)) 
                        continue;
                }
                this->getXrouterConfig(pnode, s.second.addr.ToString());
                break;
            }
        }
    }
    
    return "Config requests have been sent";
}

std::string App::printConfigs()
{
    Array result;
    
    for (const auto& it : this->snodeConfigs) {
        Object val;
        //val.emplace_back("node", it.first);
        val.emplace_back("config", it.second.rawText());
        result.push_back(Value(val));
    }
    
    return json_spirit::write_string(Value(result), true);
}

//*****************************************************************************
//*****************************************************************************
bool App::stop()
{
    server->closeAllPaymentChannels();
    return true;
}
 
//*****************************************************************************
//*****************************************************************************
std::vector<CNode*> App::getAvailableNodes(enum XRouterCommand command, std::string wallet, int confirmations)
{
    // Send only to the service nodes that have the required wallet
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();

    openConnections();
    updateConfigs();
    
    std::vector<CNode*> selectedNodes;
    
    LOCK(cs_vNodes);
    double maxfee_d = xrouter_settings.getMaxFee(command, wallet);
    CAmount maxfee;
    if (maxfee_d >= 0)
        maxfee = to_amount(maxfee_d);
    else
        maxfee = -1;
    
    int supported = 0;
    int ready = 0;
    int below_maxfee = 0;
    BOOST_FOREACH(const std::string key, snodeConfigs | boost::adaptors::map_keys)
    {
        XRouterSettings settings = snodeConfigs[key];
        if (!settings.walletEnabled(wallet))
            continue;
        if (!settings.isAvailableCommand(command, wallet))
            continue;
        
        supported++;
        CNode* res = NULL;
        for (CNode* pnode : vNodes) {
            if (key == pnode->addr.ToString()) {
                // This is the node whose config we are looking at now
                res = pnode;
                break;
            }
        }
        
        // If the service node is not among peers, we try to connect to it right now
        if (!res) {
            CAddress addr;
            res = ConnectNode(addr, key.c_str());
        }
        
        // Could not connect to service node
        if (!res)
            continue;
        
        std::string id = key;
        BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {        
            if (s.second.addr.ToString() == res->addr.ToString())
                id = CBitcoinAddress(s.second.pubKeyCollateralAddress.GetID()).ToString();
        }
        
        if (maxfee >= 0) {
            CAmount fee = to_amount(settings.getCommandFee(command, wallet));
            if (fee > maxfee) {
                LOG() << "Skipping node " << id << " because its fee=" << fee << " is higher than maxfee=" << maxfee;
                continue;
            }
        }
        
        below_maxfee++;
        
        // If connector is not working, the wallet will be removed from serviceping
        BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {        
            if (s.second.addr.ToString() == res->addr.ToString())
                if (!s.second.HasService(wallet))
                    continue;
        }
        
        std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
        std::string keystr = wallet + "::" + XRouterCommand_ToString(command);
        double timeout = settings.getCommandTimeout(command, wallet);
        if (lastPacketsSent.count(res)) {
            if (lastPacketsSent[res].count(keystr)) {
                std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsSent[res][keystr];
                std::chrono::system_clock::duration diff = time - prev_time;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                    LOG() << "Skipping node " << id << " because not enough time passed since the last call";
                    continue;
                }
            }
        }
        
        ready++;
        selectedNodes.push_back(res);
    }
    
    for (CNode* node: selectedNodes) {
        if (!snodeScore.count(node))
            snodeScore[node] = 0;
    }
    
    std::sort(selectedNodes.begin(), selectedNodes.end(), cmpNodeScore);
    
    if (supported < confirmations)
        throw XRouterError("Could not find " + std::to_string(confirmations) + " Service Nodes supporting blockchain " + wallet, xrouter::UNSUPPORTED_BLOCKCHAIN);
    
    if (below_maxfee < confirmations)
        throw XRouterError("Could not find " + std::to_string(confirmations) + " Service Nodes with fee below maxfee.", xrouter::MAXFEE_TOO_LOW);
    
    if (ready < confirmations)
        throw XRouterError("Could not find " + std::to_string(confirmations) + " Service Nodes ready for query at the moment.", xrouter::NOT_ENOUGH_NODES);
    
    return selectedNodes;
}

CNode* App::getNodeForService(std::string name)
{
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();
    
    // TODO: this is a temporary solution. We need it to open connections to snodes before XRouter calls in case they are not among peers
    openConnections();
    updateConfigs();

    double maxfee_d = xrouter_settings.getMaxFee(xrCustomCall, "");
    CAmount maxfee;
    if (maxfee_d >= 0)
        maxfee = to_amount(maxfee_d);
    else
        maxfee = -1;
    
    CNode* res = NULL;
    if (name.find("/") != string::npos) {
        std::vector<std::string> parts;
        boost::split(parts, name, boost::is_any_of("/"));
        std::string domain = parts[0];
        std::string name_part = parts[1];
        if (snodeDomains.count(domain) == 0)
            // Domain not found
            return NULL;
        
        std::vector<CNode*> candidates;
        for (CNode* pnode : vNodes) {
            XRouterSettings settings = snodeConfigs[snodeDomains[domain]];
            if (!settings.hasPlugin(name_part))
                continue;
            
            if (snodeDomains[domain] == pnode->addr.ToString()) {
                candidates.push_back(pnode);
            }
        }
        
        if (candidates.size() == 0)
            return NULL;
        else if (candidates.size() == 1)
            res = candidates[0];
        else {
            // Perform verification check of domain names
            int best_block = -1;
            for (CNode* cand : candidates) {
                std::string addr = getPaymentAddress(cand);
                int block;
                XRouterSettings settings = snodeConfigs[snodeDomains[domain]];
                std::string tx = settings.get<std::string>("Main.domain_tx", "");
                if (tx == "")
                    continue;
                
                if (!verifyDomain(tx, domain, addr, block))
                    continue;
                if ((best_block < 0) || (block < best_block)) {
                    // TODO: what if both verification tx are in the same block?
                    best_block = block;
                    res = cand;
                }
            }
            
            if (!res)
                return NULL;
        }
        
        XRouterSettings settings = snodeConfigs[snodeDomains[domain]];
        std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
        double timeout = settings.getPluginSettings(name).get<double>("timeout", -1.0);
        if (lastPacketsSent.count(res)) {
            if (lastPacketsSent[res].count(name)) {
                std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsSent[res][name];
                std::chrono::system_clock::duration diff = time - prev_time;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                    return NULL;
                }
            }
        }
        
        if (maxfee >= 0) {
            CAmount fee = to_amount(settings.getPluginSettings(name).getFee());
            if (fee > maxfee)
                return NULL;
        }
        
        return res;
    }
    
    LOCK(cs_vNodes);
    BOOST_FOREACH(const std::string key, snodeConfigs | boost::adaptors::map_keys)
    {
        XRouterSettings settings = snodeConfigs[key];
        if (!settings.hasPlugin(name))
            continue;

        bool found = false;
        for (CNode* pnode : vNodes) {
            if (key == pnode->addr.ToString()) {
                // This node is a running xrouter
                if (found) {
                    LOG() << "Ambiguous plugin call";
                    return NULL;
                }
                res = pnode;
                found = true;
            }
        }
        
        if (!res) {
            CAddress addr;
            res = ConnectNode(addr, key.c_str());
        }
        
        if (!res)
            continue;
        
        std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
        double timeout = settings.getPluginSettings(name).get<double>("timeout", -1.0);
        if (lastPacketsSent.count(res)) {
            if (lastPacketsSent[res].count(name)) {
                std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsSent[res][name];
                std::chrono::system_clock::duration diff = time - prev_time;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                    continue;
                }
            }
        }
        
        if (maxfee >= 0) {
            CAmount fee = to_amount(settings.getPluginSettings(name).getFee());
            if (fee > maxfee)
                continue;
        }
        
        return res;
    }
    
    return NULL;
}

std::string App::processGetXrouterConfig(XRouterSettings cfg, std::string addr)
{
    Object result;
    result.emplace_back(Pair("config", cfg.rawText()));
    Object plugins;
    for (std::string s : cfg.getPlugins())
        plugins.emplace_back(s, cfg.getPluginSettings(s).rawText());
    result.emplace_back(Pair("plugins", plugins));
    result.emplace_back(Pair("addr", addr));
    LOG() << "Sending config " << json_spirit::write_string(Value(result), true);
    return json_spirit::write_string(Value(result), true);
}

//*****************************************************************************
//*****************************************************************************
bool App::processReply(XRouterPacketPtr packet, CNode* node)
{
    uint32_t offset = 0;

    std::string uuid((const char *)packet->data()+offset);
    offset += uuid.size() + 1;
    std::string reply((const char *)packet->data()+offset);
    offset += reply.size() + 1;

    LOG() << "Got reply to query " << uuid;
    
    // check uuid is in queriesLock keys
    if (!queriesLocks.count(uuid))
        return true;
    
    Object ret;
    Value reply_val;
    read_string(reply, reply_val);
    
    if (reply_val.type() == obj_type) {
        Object reply_obj = reply_val.get_obj();
        const Value & code  = find_value(reply_obj, "code");
        if (code.get_int() == xrouter::EXPIRED_PAYMENT_CHANNEL) {
            std::string addr = getPaymentAddress(node);
            if (paymentChannels.count(addr))
                paymentChannels.erase(this->paymentChannels.find(addr));
        }
    }
    
    
    LOG() << reply;
    boost::mutex::scoped_lock l(*queriesLocks[uuid].first);
    if (!queries.count(uuid))
        queries[uuid] = boost::container::map<CNode*, std::string>();
    queries[uuid][node] = reply;
    queriesLocks[uuid].second->notify_all();
    return true;
}

bool App::processConfigReply(XRouterPacketPtr packet)
{
    uint32_t offset = 0;

    std::string uuid((const char *)packet->data()+offset);
    offset += uuid.size() + 1;
    std::string reply((const char *)packet->data()+offset);
    offset += reply.size() + 1;

    LOG() << "Got reply to query " << uuid;
    LOG() << "Got xrouter config from node " << configQueries[uuid]->addrName;
    LOG() << reply;
    Value reply_val;
    read_string(reply, reply_val);
    Object reply_obj = reply_val.get_obj();
    std::string config = find_value(reply_obj, "config").get_str();
    Object plugins  = find_value(reply_obj, "plugins").get_obj();
    
    XRouterSettings settings;
    settings.read(config);

    for (Object::size_type i = 0; i != plugins.size(); i++ ) {
        XRouterPluginSettings psettings;
        psettings.read(std::string(plugins[i].value_.get_str()));
        settings.addPlugin(std::string(plugins[i].name_), psettings);
    }
    if (configQueries.count(uuid)) {
        // This is a reply from service node itself
        snodeConfigs[configQueries[uuid]->addr.ToString()] = settings;
        
        // Add IP and possibly domain name to table of domains
        snodeDomains[configQueries[uuid]->addr.ToString()] = configQueries[uuid]->addr.ToString();
        if (settings.get<std::string>("Main.domain", "") != "") {
            snodeDomains[settings.get<std::string>("Main.domain")] = configQueries[uuid]->addr.ToString();
        }
        return true;
    } else {
        // This is a reply from another client node
        
        std::string addr = find_value(reply_obj, "addr").get_str();
        snodeConfigs[addr] = settings;
        snodeDomains[addr] = addr;
        if (settings.get<std::string>("Main.domain", "") != "") {
            snodeDomains[settings.get<std::string>("Main.domain")] = addr;
        }
        return true;
    }
}

//*****************************************************************************
//*****************************************************************************
void App::onMessageReceived(CNode* node, const std::vector<unsigned char>& message, CValidationState& state)
{
    LOG() << "Received xrouter packet";

    // If xrouter == 0, xrouter is turned off on this snode
    if (!isEnabled())
        return;
    
    XRouterPacketPtr packet(new XRouterPacket);
    if (!packet->copyFrom(message)) {
        LOG() << "incorrect packet received " << __FUNCTION__;
        state.DoS(10, error("XRouter: invalid packet received"), REJECT_INVALID, "xrouter-error");
        return;
    }

    std::string reply;
    LOG() << "XRouter command: " << std::string(XRouterCommand_ToString(packet->command()));
    
    std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
    if (packet->command() == xrGetXrouterConfig) {
        uint32_t offset = 36;
        std::string uuid((const char *)packet->data()+offset);
        offset += uuid.size() + 1;
        std::string addr((const char *)packet->data()+offset);
        XRouterSettings cfg;
        if (addr == "self")
            cfg = this->xrouter_settings;
        else {
            if (!this->snodeConfigs.count(addr))
                return;
            else
                cfg = this->snodeConfigs[addr];
        }
        
        reply = processGetXrouterConfig(cfg, addr);
        if (lastConfigQueries.count(node)) {
            std::chrono::time_point<std::chrono::system_clock> prev_time = lastConfigQueries[node];
            std::chrono::system_clock::duration diff = time - prev_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(diff) < std::chrono::seconds(10))
                state.DoS(10, error("XRouter: too many config requests"), REJECT_INVALID, "xrouter-error");
            lastConfigQueries[node] = time;
        } else {
            lastConfigQueries[node] = time;
        }
        
        XRouterPacketPtr rpacket(new XRouterPacket(xrConfigReply));
        rpacket->append(uuid);
        rpacket->append(reply);
        node->PushMessage("xrouter", rpacket->body());
        return;
    } else if (packet->command() == xrReply) {
        processReply(packet, node);
        return;
    } else if (packet->command() == xrConfigReply) {
        processConfigReply(packet);
        return;
    } else {
        server->onMessageReceived(node, packet, state);
        return;
    }
}

//*****************************************************************************
//*****************************************************************************
std::string App::xrouterCall(enum XRouterCommand command, const std::string & currency, std::string param1, std::string param2, std::string confirmations)
{
    std::string id = "";
    try {
        if (!isEnabled())
            throw XRouterError("XRouter is turned off. Please set 'xrouter=1' in blocknetdx.conf to run XRouter.", xrouter::UNAUTHORIZED);
        
        updateConfigs();

        uint256 txHash;
        uint32_t vout = 0;
        CKey key;
        if ((command != xrGetXrouterConfig) && !satisfyBlockRequirement(txHash, vout, key)) {
            throw XRouterError("Minimum block requirement not satisfied. Make sure that your wallet is unlocked.", xrouter::INSUFFICIENT_FUNDS);
        }

        // Check param1
        switch (command) {
            case xrGetBlockHash:
            case xrGetAllBlocks:
                if (!is_number(param1))
                    throw XRouterError("Incorrect block number: " + param1, xrouter::INVALID_PARAMETERS);
                break;
            case xrGetTransactionsBloomFilter:
                if (!is_hash(param1) || (param1.size() % 10 != 0))
                    throw XRouterError("Incorrect bloom filter: " + param1, xrouter::INVALID_PARAMETERS);
                break;
            case xrGetBlock:
            case xrGetTransaction:
                if (!is_hash(param1))
                    throw XRouterError("Incorrect hash: " + param1, xrouter::INVALID_PARAMETERS);
                break;
            case xrGetAllTransactions:
            case xrGetBalanceUpdate:
                if (!is_address(param1))
                    throw XRouterError("Incorrect address: " + param1, xrouter::INVALID_PARAMETERS);
                break;
            default:
                break;
        }
        
        // Check param2
        switch (command) {
            case xrGetAllTransactions:
            case xrGetBalanceUpdate:
            case xrGetTransactionsBloomFilter:
                if (!is_number(param2))
                    throw XRouterError("Incorrect block number: " + param2, xrouter::INVALID_PARAMETERS);
                break;
            default:
                break;
        }
        
        id = generateUUID();
        int confirmations_count = 0;
        if (confirmations != "") {
            if (!is_number(confirmations))
                throw XRouterError("Incorrect number of service nodes for consensus: " + confirmations, xrouter::INVALID_PARAMETERS);
            confirmations_count = std::stoi(confirmations);
            if (confirmations_count < 1)
                throw XRouterError("Incorrect number of service nodes for consensus: " + confirmations, xrouter::INVALID_PARAMETERS);
        }
        if (confirmations_count < 1)
            confirmations_count = xrouter_settings.get<int>("Main.consensus_nodes", 0);
        if (confirmations_count < 1)
            confirmations_count = XROUTER_DEFAULT_CONFIRMATIONS;

        Object error;
        boost::shared_ptr<boost::mutex> m(new boost::mutex());
        boost::shared_ptr<boost::condition_variable> cond(new boost::condition_variable());
        boost::mutex::scoped_lock lock(*m);
        int timeout = this->xrouter_settings.get<int>("Main.wait", XROUTER_DEFAULT_WAIT);
        LOG() << "Sending query " << id;
        queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m, cond);
        
        std::vector<CNode*> selectedNodes = getAvailableNodes(command, currency, confirmations_count);
        
        if ((int)selectedNodes.size() < confirmations_count)
            throw XRouterError("Could not find " + std::to_string(confirmations_count) + " Service Nodes to query at the moment.", xrouter::NOT_ENOUGH_NODES);
        
        bool usehash = xrouter_settings.get<int>("Main.usehash", 0) != 0;
        if (confirmations_count == 1)
            usehash = false;
        
        // Disable usehash option
        usehash = false;
        
        int cnt = 0;
        boost::container::map<CNode*, std::string > paytx_map;
        for (CNode* pnode : selectedNodes) {
            CAmount fee = to_amount(snodeConfigs[pnode->addr.ToString()].getCommandFee(command, currency));
            CAmount fee_part1 = fee;
            std::string payment_tx = usehash ? "hash;nofee" : "nohash;nofee";
            if (fee > 0) {
                try {
                    if (!usehash)
                        payment_tx = "nohash;" + generatePayment(pnode, fee);
                    else {
                        fee_part1 = fee / 2;
                        payment_tx = "hash;" + generatePayment(pnode, fee_part1);
                    }
                } catch (std::runtime_error e) {
                    LOG() << "Failed to create payment to node " << pnode->addr.ToString();
                    continue;
                }
            }
            
            paytx_map[pnode] = payment_tx;
            cnt++;
            if (cnt == confirmations_count)
                break;
        }
        
        if (cnt < confirmations_count) {
            for (CNode* pnode : selectedNodes) {
                if (paytx_map.count(pnode))
                    unlockOutputs(paytx_map[pnode]);
            }
            
            throw XRouterError("Could not create payments to service nodes. Please check that your wallet is fully unlocked and you have at least " + std::to_string(confirmations_count) + " available unspent transaction outputs.", xrouter::INSUFFICIENT_FUNDS);
            
            /*boost::container::map<std::string, CAmount> addr_map;
            cnt = 0;
            paytx_map.clear();
            for (CNode* pnode : selectedNodes) {
                CAmount fee = to_amount(snodeConfigs[pnode->addr.ToString()].getCommandFee(command, currency));
                CAmount fee_part1 = fee;
                if (fee > 0) {
                    if (usehash)
                        fee_part1 = fee / 2;
                }
                std::string dest = getPaymentAddress(pnode);
                addr_map[dest] = fee_part1;
                paytx_map[pnode] = "";
                cnt++;
                if (cnt == confirmations_count)
                    break;
            }
            
            std::string payment_tx;
            if (!usehash)
                payment_tx = "nohash;single;";
            else
                payment_tx = "hash;single;";
            try {
                std::string tx;
                createAndSignTransaction(addr_map, tx);
                payment_tx += tx;
                for (CNode* pnode : selectedNodes) {
                    if (paytx_map.count(pnode))
                        paytx_map[pnode] = payment_tx;
                }
            } catch (std::runtime_error e) {
                LOG() << "Failed to create payment";
                throw XRouterError("Could not create payments to service nodes", xrouter::INSUFFICIENT_FUNDS);
            }*/
        }
            
        for (CNode* pnode : selectedNodes) {
            if (!paytx_map.count(pnode))
                continue;
            XRouterPacketPtr packet(new XRouterPacket(command));
            packet->append(txHash.begin(), 32);
            packet->append(vout);
            packet->append(id);
            packet->append(currency);
            packet->append(paytx_map[pnode]);
            if (!param1.empty())
                packet->append(param1);
            if (!param2.empty())
                packet->append(param2);
            packet->sign(key);
            
            pnode->PushMessage("xrouter", packet->body());
            
            std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
            std::string keystr = currency + "::" + XRouterCommand_ToString(packet->command());
            if (!lastPacketsSent.count(pnode)) {
                lastPacketsSent[pnode] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
            }
            lastPacketsSent[pnode][keystr] = time;
            LOG() << "Sent message to node " << pnode->addrName;
        }

        int confirmation_count = 0;
        while ((confirmation_count < confirmations_count) && cond->timed_wait(lock, boost::posix_time::milliseconds(timeout)))
            confirmation_count++;

        std::string result = "";
        if(confirmation_count <= confirmations_count / 2) {
            throw XRouterError("Failed to get response in time. Try xrGetReply command later.", xrouter::SERVER_TIMEOUT);
        }
        else {
            BOOST_FOREACH( queries_map::value_type &i, queries[id] ) {
                std::string cand = i.second;
                int cnt = 0;
                BOOST_FOREACH( queries_map::value_type &j, queries[id] ) {
                    if (j.second == cand) {
                        cnt++;
                        if (cnt > confirmations_count / 2)
                            result = cand;
                    }
                }
                
            }
            
            
            if (result.empty()) {
                throw XRouterError("No consensus between responses", xrouter::INTERNAL_SERVER_ERROR);
            } else {
                if (!usehash)
                    return result;
            }
        }
        
        // We reach here only if usehash == true
        CNode* finalnode;
        BOOST_FOREACH( queries_map::value_type &i, queries[id] ) {
            if (result == i.second) {
                finalnode = i.first;
                break;
            }
        }
        
        CAmount fee = to_amount(snodeConfigs[finalnode->addr.ToString()].getCommandFee(command, currency));
        CAmount fee_part2 = fee - fee/2;
        std::string payment_tx = "nofee";
        if (fee > 0) {
            try {
                payment_tx = "nohash;" + generatePayment(finalnode, fee_part2);
            } catch (std::runtime_error) {
                LOG() << "Failed to create payment to node " << finalnode->addr.ToString();
                throw XRouterError("Could not create payment to service node", xrouter::INSUFFICIENT_FUNDS);
            }
        }
        XRouterPacketPtr fpacket(new XRouterPacket(xrFetchReply));
        fpacket->append(txHash.begin(), 32);
        fpacket->append(vout);
        fpacket->append(id);
        fpacket->append(currency);
        fpacket->append(payment_tx);
        fpacket->sign(key);

        finalnode->PushMessage("xrouter", fpacket->body());

        LOG() << "Fetching reply from node " << finalnode->addrName;

        boost::shared_ptr<boost::mutex> m2(new boost::mutex());
        boost::shared_ptr<boost::condition_variable> cond2(new boost::condition_variable());
        boost::mutex::scoped_lock lock2(*m2);
        queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m2, cond2);
        if (cond2->timed_wait(lock2, boost::posix_time::milliseconds(timeout))) {
            std::string reply = queries[id][finalnode];
            return reply;
        } else {
            throw XRouterError("Failed to fetch reply from service node", xrouter::SERVER_TIMEOUT);
        }
    } catch (XRouterError e) {
        Object error;
        error.emplace_back(Pair("error", e.msg));
        error.emplace_back(Pair("code", e.code));
        error.emplace_back(Pair("uuid", id));
        LOG() << e.msg;
        return json_spirit::write_string(Value(error), true);
    }
}

std::string App::getBlockCount(const std::string & currency, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBlockCount, currency, "", "", confirmations);
}

std::string App::getBlockHash(const std::string & currency, const std::string & blockId, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBlockHash, currency, blockId, "", confirmations);
}

std::string App::getBlock(const std::string & currency, const std::string & blockHash, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBlock, currency, blockHash, "", confirmations);
}

std::string App::getTransaction(const std::string & currency, const std::string & hash, const std::string & confirmations)
{
    return this->xrouterCall(xrGetTransaction, currency, hash, "", confirmations);
}

std::string App::getAllBlocks(const std::string & currency, const std::string & number, const std::string & confirmations)
{
    return this->xrouterCall(xrGetAllBlocks, currency, number, "", confirmations);
}

std::string App::getAllTransactions(const std::string & currency, const std::string & account, const std::string & number, const std::string & confirmations)
{
    return this->xrouterCall(xrGetAllTransactions, currency, account, number, confirmations);
}

std::string App::getBalance(const std::string & currency, const std::string & account, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBalance, currency, account, "", confirmations);
}

std::string App::getBalanceUpdate(const std::string & currency, const std::string & account, const std::string & number, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBalanceUpdate, currency, account, number, confirmations);
}

std::string App::getTransactionsBloomFilter(const std::string & currency, const std::string & filter, const std::string & number, const std::string & confirmations)
{
    return this->xrouterCall(xrGetTransactionsBloomFilter, currency, filter, number, confirmations);
}

std::string App::convertTimeToBlockCount(const std::string& currency, std::string time, const std::string& confirmations) {

    return this->xrouterCall(xrTimeToBlockNumber, currency, time, "", confirmations);
}

std::string App::getReply(const std::string & id)
{
    Object result;

    if(queries[id].size() == 0) {
        result.emplace_back(Pair("error", "No replies found"));
        result.emplace_back(Pair("uuid", id));
        return json_spirit::write_string(Value(result), true);
    } else {
        BOOST_FOREACH( queries_map::value_type &it, queries[id] ) {
            std::string cand = it.second;
            // TODO: display node id
            result.emplace_back(Pair("reply", cand));
        }

        return json_spirit::write_string(Value(result), true);
    }
}

std::string App::sendTransaction(const std::string & currency, const std::string & transaction)
{
    return this->xrouterCall(xrSendTransaction, currency, transaction, "", "1");
}

std::string App::sendCustomCall(const std::string & name, std::vector<std::string> & params)
{
    std::string id;
    try {
        if (!isEnabled())
            throw XRouterError("XRouter is turned off. Please set 'xrouter=1' in blocknetdx.conf to run XRouter.", xrouter::UNAUTHORIZED);
        
        if (this->xrouter_settings.hasPlugin(name)) {
            // Run the plugin locally
            return server->processCustomCall(name, params);
        }
        
        updateConfigs();
        
        XRouterPacketPtr packet(new XRouterPacket(xrCustomCall));

        uint256 txHash;
        uint32_t vout = 0;
        CKey key;
        if (!satisfyBlockRequirement(txHash, vout, key)) {
            throw XRouterError("Minimum block requirement not satisfied. Make sure that your wallet is unlocked.", xrouter::INSUFFICIENT_FUNDS);
        }
        
        id = generateUUID();

        boost::shared_ptr<boost::mutex> m(new boost::mutex());
        boost::shared_ptr<boost::condition_variable> cond(new boost::condition_variable());
        boost::mutex::scoped_lock lock(*m);
        queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m, cond);
        
        CNode* pnode = getNodeForService(name);
        if (!pnode)
            throw XRouterError("Plugin not found", xrouter::UNSUPPORTED_SERVICE);
        
        unsigned int min_count = snodeConfigs[pnode->addr.ToString()].getPluginSettings(name).getMinParamCount();
        if (params.size() < min_count) {
            throw XRouterError("Not enough plugin parameters", xrouter::INVALID_PARAMETERS);
        }
        
        unsigned int max_count = snodeConfigs[pnode->addr.ToString()].getPluginSettings(name).getMaxParamCount();
        if (params.size() > max_count) {
            throw XRouterError("Too many plugin parameters", xrouter::INVALID_PARAMETERS);
        }
        
        std::string strtxid;
        CAmount fee = to_amount(snodeConfigs[pnode->addr.ToString()].getPluginSettings(name).getFee());
        std::string payment_tx = "nofee";
        if (fee > 0) {
            try {
                payment_tx = "nohash;" + generatePayment(pnode, fee);
                LOG() << "Payment transaction: " << payment_tx;
                //std::cout << "Payment transaction: " << payment_tx << std::endl << std::flush;
            } catch (std::runtime_error) {
                LOG() << "Failed to create payment to node " << pnode->addr.ToString();
            }
        }
        
        packet->append(txHash.begin(), 32);
        packet->append(vout);
        packet->append(id);
        packet->append(name);
        packet->append(payment_tx);
        for (std::string param: params)
            packet->append(param);
        packet->sign(key);
        std::vector<unsigned char> msg;
        msg.insert(msg.end(), packet->body().begin(), packet->body().end());
        
        Object result;
        
        std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
        if (!lastPacketsSent.count(pnode)) {
            lastPacketsSent[pnode] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
        }
        lastPacketsSent[pnode][name] = time;
        
        pnode->PushMessage("xrouter", msg);
        int timeout = this->xrouter_settings.get<int>("Main.wait", XROUTER_DEFAULT_WAIT);
        if (cond->timed_wait(lock, boost::posix_time::milliseconds(timeout))) {
            std::string reply = queries[id][pnode];
            return reply;
        }
        
        throw XRouterError("Failed to get response in time. Try xrGetReply command later.", xrouter::SERVER_TIMEOUT);
    } catch (XRouterError e) {
        Object error;
        error.emplace_back(Pair("error", e.msg));
        error.emplace_back(Pair("code", e.code));
        error.emplace_back(Pair("uuid", id));
        LOG() << e.msg;
        return json_spirit::write_string(Value(error), true);
    }
}

std::string App::generatePayment(CNode* pnode, CAmount fee)
{
    std::string strtxid;
    std::string dest = getPaymentAddress(pnode);
    CAmount deposit = to_amount(xrouter_settings.get<double>("Main.deposit", 0.0));
    int channeldate = xrouter_settings.get<int>("Main.channeldate", 100000);
    std::string deposit_pubkey_s = snodeConfigs[pnode->addr.ToString()].get<std::string>("depositpubkey", "");
    std::string payment_tx = "nofee";
    bool res;
    if (fee > 0) {
        if ((deposit == 0) || (deposit_pubkey_s == "")) {
            res = createAndSignTransaction(dest, fee, payment_tx);
            payment_tx = "single;" + payment_tx;
            if(!res) {
                throw std::runtime_error("Failed to create payment transaction");
            }
        } else {
            // Create payment channel first
            std::string raw_tx, txid;
            payment_tx = "";
            PaymentChannel channel;
            std::string addr = getPaymentAddress(pnode);
            CPubKey deposit_pubkey = CPubKey(ParseHex(deposit_pubkey_s));
            
            // Clear expired channel
            if (this->paymentChannels.count(addr)) {
                if (std::time(0) >= this->paymentChannels[addr].deadline) {
                    this->paymentChannels.erase(this->paymentChannels.find(addr));
                }
            }
            
            if (!this->paymentChannels.count(addr)) {
                channel = createPaymentChannel(deposit_pubkey, deposit, channeldate);
                if (channel.txid == "")
                    throw std::runtime_error("Failed to create payment channel");
                this->paymentChannels[addr] = channel;
                payment_tx = channel.raw_tx + ";" + channel.txid + ";" + HexStr(channel.redeemScript.begin(), channel.redeemScript.end()) + ";";
            }
            
            // Submit payment via channel
            CAmount paid = this->paymentChannels[addr].value;
            
            std::string paytx;
            bool res = createAndSignChannelTransaction(this->paymentChannels[addr], dest, deposit, fee + paid, paytx);
            if (!res)
                throw std::runtime_error("Failed to pay to payment channel");
            this->paymentChannels[addr].latest_tx = paytx;
            this->paymentChannels[addr].value = fee + paid;
            
            // Send channel tx, channel tx id, payment tx in one string
            payment_tx += paytx;
            payment_tx = "channel;" + payment_tx;
        }
        
        LOG() << "Payment transaction: " << payment_tx;
        //std::cout << "Payment transaction: " << payment_tx << std::endl << std::flush;
        savePaymentChannels();
    }
    
    return payment_tx;
}

std::string App::getPaymentAddress(CNode* node)
{
    // Payment address = pubkey Collateral address of snode
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();
    BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {
        if (s.second.addr.ToString() == node->addr.ToString()) {
            return CBitcoinAddress(s.second.pubKeyCollateralAddress.GetID()).ToString();
        }
    }
    
    if (debug_on_client())
        return "yBW61mwkjuqFK1rVfm2Az2s2WU5Vubrhhw";
    
    return "";
}

CPubKey App::getPaymentPubkey(CNode* node)
{
    // Payment address = pubkey Collateral address of snode
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();
    BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {
        if (s.second.addr.ToString() == node->addr.ToString()) {
            return s.second.pubKeyCollateralAddress;
        }
    }
    
    if (debug_on_client()) {
        std::string test = "03872bfe748a5a3868c74c8f820ed1387a58d48c67a7c415c7b3fad1ca61803365";
        return CPubKey(ParseHex(test));
    }
    
    return CPubKey();
}

std::string App::printPaymentChannels() {
    Array client;
    
    for (const auto& it : this->paymentChannels) {
        Object val;
        val.emplace_back("Node id", it.first);
        val.emplace_back("Deposit transaction", it.second.raw_tx);
        val.emplace_back("Deposit transaction id", it.second.txid);
        val.emplace_back("Redeem transaction", it.second.latest_tx);
        val.emplace_back("Paid amount", it.second.value);
        val.emplace_back("Expires in (ms):", it.second.deadline - std::time(0));
        client.push_back(Value(val));
    }
    
    Object result;
    result.emplace_back("Client side", client);
    result.emplace_back("Server side", this->server->printPaymentChannels());
    
    return json_spirit::write_string(Value(result), true);
}

std::string App::getXrouterConfig(CNode* node, std::string addr) {
    XRouterPacketPtr packet(new XRouterPacket(xrGetXrouterConfig));

    uint256 txHash;
    uint32_t vout = 0;

    std::string id = generateUUID();
    packet->append(txHash.begin(), 32);
    packet->append(vout);
    packet->append(id);
    packet->append(addr);
    
    std::vector<unsigned char> msg;
    msg.insert(msg.end(), packet->body().begin(), packet->body().end());
    
    this->configQueries[id] = node;
    node->PushMessage("xrouter", msg);
    return id;
}

std::string App::getXrouterConfigSync(CNode* node) {
    XRouterPacketPtr packet(new XRouterPacket(xrGetXrouterConfig));

    uint256 txHash;
    uint32_t vout = 0;

    std::string id = generateUUID();

    packet->append(txHash.begin(), 32);
    packet->append(vout);
    packet->append(id);

    boost::shared_ptr<boost::mutex> m(new boost::mutex());
    boost::shared_ptr<boost::condition_variable> cond(new boost::condition_variable());
    boost::mutex::scoped_lock lock(*m);
    queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m, cond);

    std::vector<unsigned char> msg;
    msg.insert(msg.end(), packet->body().begin(), packet->body().end());
    node->PushMessage("xrouter", msg);
    int timeout = this->xrouter_settings.get<int>("Main.wait", XROUTER_DEFAULT_WAIT);
    if (!cond->timed_wait(lock, boost::posix_time::milliseconds(timeout)))
        return "Could not get XRouter config";

    std::string reply = queries[id][node];
    XRouterSettings settings;
    settings.read(reply);
    this->snodeConfigs[node->addr.ToString()] = settings;
    return reply;
}

void App::reloadConfigs() {
    LOG() << "Reloading xrouter config from file " << xrouterpath;
    this->xrouter_settings.read(xrouterpath.c_str());
    this->xrouter_settings.loadPlugins();
}

std::string App::getStatus() {
    Object result;
    result.emplace_back(Pair("enabled", isEnabled()));
    result.emplace_back(Pair("config", this->xrouter_settings.rawText()));
    Object myplugins;
    for (std::string s : this->xrouter_settings.getPlugins())
        myplugins.emplace_back(s, this->xrouter_settings.getPluginSettings(s).rawText());
    result.emplace_back(Pair("plugins", myplugins));
    
    Array nodes;
    for (auto& it : this->snodeConfigs) {
        Object val;
        //val.emplace_back("node", it.first);
        val.emplace_back("config", it.second.rawText());
        Object plugins;
        for (std::string s : it.second.getPlugins())
            plugins.emplace_back(s, it.second.getPluginSettings(s).rawText());
        val.emplace_back(Pair("plugins", plugins));
        nodes.push_back(Value(val));
    }
    
    result.emplace_back(Pair("nodes", nodes));
    
    return json_spirit::write_string(Value(result), true);
}

void App::closePaymentChannel(std::string id) {
    server->closePaymentChannel(id);
}

void App::closeAllPaymentChannels() {
    server->closeAllPaymentChannels();
}

void App::savePaymentChannels() {
    std::string path(GetDataDir(true).string() + "/paymentchannels.json");
    Array client;
    
    for (const auto& it : this->paymentChannels) {
        Object val;
        val.emplace_back("id", it.first);
        val.emplace_back("raw_tx", it.second.raw_tx);
        val.emplace_back("txid", it.second.txid);
        val.emplace_back("vout", it.second.vout);
        val.emplace_back("latest_tx", it.second.latest_tx);
        val.emplace_back("value", it.second.value);
        val.emplace_back("deposit", it.second.deposit);
        val.emplace_back("deadline", it.second.deadline);
        val.emplace_back("keyid", it.second.keyid.ToString());
        val.emplace_back("redeemScript", HexStr(it.second.redeemScript.begin(), it.second.redeemScript.end()) );
        client.push_back(Value(val));
    }
    
    ofstream f(path);
    f << json_spirit::write_string(Value(client), true);
    f.close();
}

void App::loadPaymentChannels() {
    std::string path(GetDataDir(true).string() + "/paymentchannels.json");
    ifstream f(path);
    std::string config;
    f >> config;
    f.close();
    
    Value data;
    read_string(config, data);
    
    if (data.type() != array_type)
        return;
    
    Array channels = data.get_array();
    
    for (size_t i = 0; i < channels.size(); i++) {
        if (channels[i].type() != obj_type)
            continue;
        
        try {
            Object channel = channels[i].get_obj();
            std::string id = find_value(channel, "id").get_str();
            
            PaymentChannel c;
            c.raw_tx = find_value(channel, "raw_tx").get_str();
            c.txid = find_value(channel, "txid").get_str();
            c.vout = find_value(channel, "vout").get_int();
            c.latest_tx = find_value(channel, "latest_tx").get_str();
            c.value = to_amount(find_value(channel, "value").get_int());
            c.deposit = to_amount(find_value(channel, "deposit").get_int());
            c.deadline = find_value(channel, "deadline").get_int();
            std::vector<unsigned char> script = ParseHex(find_value(channel, "redeemScript").get_str());
            c.redeemScript = CScript(script.begin(), script.end());
            CBitcoinAddress addr = CBitcoinAddress(find_value(channel, "keyid").get_str());
            addr.GetKeyID(c.keyid);
            pwalletMain->GetKey(c.keyid, c.key);
                
            this->paymentChannels[id] = c;
        } catch (...) {
            continue;
        }
    }
}

std::string App::createDepositAddress(bool update) {
    CPubKey my_pubkey; 
    {
        LOCK(pwalletMain->cs_wallet);
        my_pubkey = pwalletMain->GenerateNewKey();
    }
    
    if (!my_pubkey.IsValid()) {
        Object error;
        error.emplace_back(Pair("error", "Could not generate deposit address. Make sure that your wallet is unlocked."));
        error.emplace_back(Pair("code", xrouter::INSUFFICIENT_FUNDS));
        error.emplace_back(Pair("uuid", ""));
        return json_spirit::write_string(Value(error), true);
    }
    
    CKeyID mykeyID = my_pubkey.GetID();
    CBitcoinAddress addr(mykeyID);
    Object result;
    result.emplace_back(Pair("depositpubkey", HexStr(my_pubkey)));
    result.emplace_back(Pair("depositaddress", addr.ToString()));
    if (update) {
        this->xrouter_settings.set("Main.depositaddress", addr.ToString());
        this->xrouter_settings.set("Main.depositpubkey", HexStr(my_pubkey));
        this->xrouter_settings.write();
    }
    return json_spirit::write_string(Value(result), true);
}

void App::runTests() {
    server->runPerformanceTests();
}

bool App::debug_on_client() {
    return xrouter_settings.get<int>("Main.debug_on_client", 0) != 0;
}

bool App::isDebug() {
    //int b;
    //verifyDomain("98fa59764df5d2022994ca98e8ad3ca795681920bb7cad0af8df07ab48539ac6", "antihype", "yBW61mwkjuqFK1rVfm2Az2s2WU5Vubrhhw", b);
    return xrouter_settings.get<int>("Main.debug", 0) != 0;
}

std::string App::getMyPaymentAddress() {
    return server->getMyPaymentAddress();
}

std::string App::registerDomain(std::string domain, std::string addr, bool update) {
    std::string result = generateDomainRegistrationTx(domain, addr);
    
    if (update) {
        this->xrouter_settings.set("Main.domain", domain);
        this->xrouter_settings.set("Main.domain_tx", result);
        this->xrouter_settings.write();
    }
    
    return result;
}
    

bool App::queryDomain(std::string domain) {
    openConnections();
    updateConfigs();
    
    if (snodeDomains.count(domain) == 0)
        return false;
    
    for (CNode* pnode : vNodes) {
        XRouterSettings settings = snodeConfigs[snodeDomains[domain]];
        
        if (snodeDomains[domain] != pnode->addr.ToString())
            continue;
        
        std::string addr = getPaymentAddress(pnode);
        int block;
        std::string tx = settings.get<std::string>("Main.domain_tx", "");
        if (tx == "")
            continue;
        
        if (verifyDomain(tx, domain, addr, block))
            return true;
    }
    
    return false;
}

} // namespace xrouter
