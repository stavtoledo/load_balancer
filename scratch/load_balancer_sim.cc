#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include <map>
#include <vector>
#include <limits>
#include <iostream>
#include <iomanip>
#include <string>
#include <fstream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("LoadBalancerSim");

enum LoadBalancingStrategy {
    ROUND_ROBIN,
    LEAST_CONNECTIONS,
    IP_HASH,
    WEIGHTED_ROUND_ROBIN
};

// ============================================================================
// Custom Load Balancer Application
// ============================================================================
class LoadBalancerApp : public Application {
public:
    LoadBalancerApp() 
        : m_port(8080), 
          m_serverPort(80), 
          m_nextServerIdx(0), 
          m_strategy(LEAST_CONNECTIONS),
          m_sessionDuration(Seconds(1.5)),
          m_totalRequests(0),
          m_droppedRequests(0),
          m_requestsThisInterval(0),
          m_currentWeightCount(0) {}

    void Setup(uint16_t listenPort, 
               const std::vector<Ipv4Address> &serverIps, 
               uint16_t serverPort, 
               LoadBalancingStrategy strategy = LEAST_CONNECTIONS) {
        m_port = listenPort;
        m_serverIps = serverIps;
        m_serverPort = serverPort;
        m_strategy = strategy;

        for (const auto &ip : m_serverIps) {
            m_activeConnections[ip] = 0;
            m_serverHealth[ip] = true;
            m_serverRequestCount[ip] = 0;
            m_serverBusyTime[ip] = Seconds(0);
            m_lastBusyStart[ip] = Seconds(0);
            m_serverProcessingTime[ip] = m_sessionDuration; // Default processing time
            m_weights[ip] = 1; // Default weight
        }
    }

    void SetQueues(std::vector<Ptr<Queue<Packet>>> queues) {
        m_queues = queues;
    }

    std::string GetStrategyName() const {
        switch (m_strategy) {
            case ROUND_ROBIN: return "ROUND_ROBIN";
            case LEAST_CONNECTIONS: return "LEAST_CONNECTIONS";
            case IP_HASH: return "IP_HASH";
            case WEIGHTED_ROUND_ROBIN: return "WEIGHTED_ROUND_ROBIN";
            default: return "UNKNOWN";
        }
    }

    void SetServerProcessingTime(Ipv4Address serverIp, Time duration) {
        m_serverProcessingTime[serverIp] = duration;
    }

    void SetServerWeight(Ipv4Address serverIp, uint32_t weight) {
        m_weights[serverIp] = weight;
    }

    void SetServerHealth(Ipv4Address serverIp, bool isHealthy) {
        m_serverHealth[serverIp] = isHealthy;
        if (!isHealthy) {
            NS_LOG_INFO("[HealthCheck] *** SERVER FAILURE DETECTED *** Server " << serverIp << " is DOWN!");
            std::cout << "\n========================================\n"
                      << "[HealthCheck] SERVER " << serverIp << " IS NOW DOWN! (t = " 
                      << Simulator::Now().GetSeconds() << "s)\n"
                      << "========================================\n" << std::endl;
            
            if (m_activeConnections[serverIp] > 0) {
                m_serverBusyTime[serverIp] += (Simulator::Now() - m_lastBusyStart[serverIp]);
            }
            m_activeConnections[serverIp] = 0;
        } else {
            NS_LOG_INFO("[HealthCheck] Server " << serverIp << " recovered and is UP!");
        }
    }

    void PrintMetricsSummary(double totalSimTime) {
        std::cout << "\n=====================================================================\n";
        std::cout << "             PHASE 5: ADVANCED METRICS EVALUATION SUMMARY            \n";
        std::cout << "             ALGORITHM: " << GetStrategyName() << "                  \n";
        std::cout << "=====================================================================\n\n";

        std::cout << "1. SYSTEM AVAILABILITY & FAULT TOLERANCE\n";
        std::cout << "---------------------------------------------------------------------\n";
        std::cout << " Total Client Requests Received : " << m_totalRequests << "\n";
        std::cout << " Total Requests Dropped         : " << m_droppedRequests << "\n";
        double availability = (m_totalRequests > 0) 
            ? ((m_totalRequests - m_droppedRequests) / (double)m_totalRequests) * 100.0 : 0.0;
        std::cout << " System Availability Rate       : " << std::fixed << std::setprecision(2) 
                  << availability << "%\n\n";

        std::cout << "2. SERVER LOAD DISTRIBUTION & UTILIZATION\n";
        std::cout << "---------------------------------------------------------------------\n";
        std::cout << std::left << std::setw(16) << "Server IP" 
                  << std::setw(18) << "Requests Handled" 
                  << std::setw(16) << "Load Share (%)" 
                  << std::setw(18) << "Server Util (%)" << "\n";
        std::cout << "---------------------------------------------------------------------\n";

        uint32_t handledRequests = m_totalRequests - m_droppedRequests;
        for (const auto &ip : m_serverIps) {
            uint32_t reqs = m_serverRequestCount[ip];
            double loadShare = (handledRequests > 0) ? (reqs / (double)handledRequests) * 100.0 : 0.0;
            
            Time finalBusyTime = m_serverBusyTime[ip];
            if (m_activeConnections[ip] > 0 && m_serverHealth[ip]) {
                finalBusyTime += (Simulator::Now() - m_lastBusyStart[ip]);
            }

            double util = (finalBusyTime.GetSeconds() / totalSimTime) * 100.0;
            if (util > 100.0) util = 100.0;

            std::ostringstream ipStr;
            ipStr << ip;

            std::cout << std::left << std::setw(16) << ipStr.str() 
                      << std::setw(18) << reqs 
                      << std::setw(16) << std::fixed << std::setprecision(2) << loadShare 
                      << std::setw(18) << std::fixed << std::setprecision(2) << util << "\n";
        }
        std::cout << "---------------------------------------------------------------------\n";
    }

private:
    virtual void StartApplication(void) override {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
        m_socket->Bind(local);
        m_socket->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleRead, this));

        NS_LOG_INFO("[LoadBalancer] Listening on port " << m_port);

        std::string filename = "time_series_" + GetStrategyName() + ".csv";
        m_csv.open(filename.c_str());
        m_csv << "Time_Seconds,Server_1_Requests,Server_2_Requests,Server_3_Requests\n";

        std::string qFilename = "queue_series_" + GetStrategyName() + ".csv";
        m_queueCsv.open(qFilename.c_str());
        m_queueCsv << "Time_Seconds,Server_1_Queue,Server_2_Queue,Server_3_Queue\n";
        
        Simulator::Schedule(Seconds(0.2), &LoadBalancerApp::LogTraffic, this);
    }

    virtual void StopApplication(void) override {
        if (m_socket) m_socket->Close();
        if (m_csv.is_open()) m_csv.close();
        if (m_queueCsv.is_open()) m_queueCsv.close();
    }

    void LogTraffic() {
        double now = Simulator::Now().GetSeconds();

        if (m_csv.is_open() && m_serverIps.size() >= 3) {
            uint32_t s1_req = m_serverRequestCount[m_serverIps[0]];
            uint32_t s2_req = m_serverRequestCount[m_serverIps[1]];
            uint32_t s3_req = m_serverRequestCount[m_serverIps[2]];
            
            m_csv << now << "," << s1_req << "," << s2_req << "," << s3_req << "\n";
        }

        if (m_queueCsv.is_open() && m_queues.size() >= 3) {
            m_queueCsv << now << "," 
                       << m_queues[0]->GetNPackets() << ","
                       << m_queues[1]->GetNPackets() << ","
                       << m_queues[2]->GetNPackets() << "\n";
        }

        Simulator::Schedule(Seconds(0.2), &LoadBalancerApp::LogTraffic, this);
    }

    void HandleRead(Ptr<Socket> socket) {
        Ptr<Packet> packet;
        Address from;

        while ((packet = socket->RecvFrom(from))) {
            m_totalRequests++;
            m_requestsThisInterval++; 

            Ipv4Address targetServer = SelectBackendServer(from);

            if (targetServer == Ipv4Address("0.0.0.0")) {
                m_droppedRequests++;
                NS_LOG_ERROR("[LoadBalancer] All backend servers DOWN! Request dropped.");
                continue;
            }

            m_serverRequestCount[targetServer]++;
            if (m_activeConnections[targetServer] == 0) {
                m_lastBusyStart[targetServer] = Simulator::Now();
            }

            m_activeConnections[targetServer]++;
            ForwardPacket(packet, targetServer);
            
            Simulator::Schedule(m_serverProcessingTime[targetServer], &LoadBalancerApp::ReleaseConnection, this, targetServer);
        }
    }

    Ipv4Address SelectBackendServer(Address from) {
        if (m_strategy == LEAST_CONNECTIONS) return SelectLeastConnections();
        if (m_strategy == IP_HASH) return SelectIpHash(from);
        if (m_strategy == WEIGHTED_ROUND_ROBIN) return SelectWeightedRoundRobin();
        return SelectRoundRobin();
    }

    Ipv4Address SelectRoundRobin() {
        size_t attempts = 0;
        while (attempts < m_serverIps.size()) {
            Ipv4Address candidate = m_serverIps[m_nextServerIdx];
            m_nextServerIdx = (m_nextServerIdx + 1) % m_serverIps.size();
            if (m_serverHealth[candidate]) return candidate;
            attempts++;
        }
        return Ipv4Address("0.0.0.0");
    }

    Ipv4Address SelectLeastConnections() {
        Ipv4Address selectedServer = Ipv4Address("0.0.0.0");
        uint32_t minConnections = std::numeric_limits<uint32_t>::max();

        for (const auto &ip : m_serverIps) {
            if (!m_serverHealth[ip]) continue;
            if (m_activeConnections[ip] < minConnections) {
                minConnections = m_activeConnections[ip];
                selectedServer = ip;
            }
        }
        return selectedServer;
    }

    Ipv4Address SelectIpHash(Address from) {
        InetSocketAddress clientAddr = InetSocketAddress::ConvertFrom(from);
        uint32_t ipAsInt = clientAddr.GetIpv4().Get();
        
        std::vector<Ipv4Address> healthyServers;
        for (const auto &ip : m_serverIps) {
            if (m_serverHealth[ip]) healthyServers.push_back(ip);
        }
        if (healthyServers.empty()) return Ipv4Address("0.0.0.0");

        uint32_t targetIdx = ipAsInt % healthyServers.size();
        return healthyServers[targetIdx];
    }

    Ipv4Address SelectWeightedRoundRobin() {
        size_t attempts = 0;
        uint32_t totalWeight = 0;
        for (const auto &pair : m_weights) {
            if(m_serverHealth[pair.first]) totalWeight += pair.second;
        }
        if (totalWeight == 0) return Ipv4Address("0.0.0.0");

        while (attempts < totalWeight * m_serverIps.size()) {
            Ipv4Address candidate = m_serverIps[m_nextServerIdx];
            
            if (m_serverHealth[candidate]) {
                m_currentWeightCount++;
                if (m_currentWeightCount >= m_weights[candidate]) {
                    m_nextServerIdx = (m_nextServerIdx + 1) % m_serverIps.size();
                    m_currentWeightCount = 0;
                }
                return candidate;
            } else {
                m_nextServerIdx = (m_nextServerIdx + 1) % m_serverIps.size();
                m_currentWeightCount = 0;
            }
            attempts++;
        }
        return Ipv4Address("0.0.0.0");
    }

    void ReleaseConnection(Ipv4Address serverIp) {
        if (m_activeConnections[serverIp] > 0) {
            m_activeConnections[serverIp]--;
            if (m_activeConnections[serverIp] == 0) {
                m_serverBusyTime[serverIp] += (Simulator::Now() - m_lastBusyStart[serverIp]);
            }
        }
    }

    void ForwardPacket(Ptr<Packet> packet, Ipv4Address targetServerIp) {
        Ptr<Socket> forwardSocket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        InetSocketAddress serverAddr = InetSocketAddress(targetServerIp, m_serverPort);
        forwardSocket->Connect(serverAddr);
        forwardSocket->Send(packet);
        forwardSocket->Close();
    }

    uint16_t m_port;
    uint16_t m_serverPort;
    Ptr<Socket> m_socket;
    std::vector<Ipv4Address> m_serverIps;
    size_t m_nextServerIdx;
    LoadBalancingStrategy m_strategy;
    
    std::map<Ipv4Address, uint32_t> m_activeConnections;
    std::map<Ipv4Address, bool> m_serverHealth;
    std::map<Ipv4Address, uint32_t> m_serverRequestCount;
    std::map<Ipv4Address, Time> m_serverBusyTime;
    std::map<Ipv4Address, Time> m_lastBusyStart;
    
    Time m_sessionDuration;
    std::map<Ipv4Address, Time> m_serverProcessingTime;
    std::map<Ipv4Address, uint32_t> m_weights;

    uint32_t m_totalRequests;
    uint32_t m_droppedRequests;
    uint32_t m_requestsThisInterval;
    uint32_t m_currentWeightCount;
    std::ofstream m_csv;
    
    std::ofstream m_queueCsv;
    std::vector<Ptr<Queue<Packet>>> m_queues;
};

// ============================================================================
// Topology & Simulation Runner
// ============================================================================
int main(int argc, char *argv[]) {
    LogComponentEnable("LoadBalancerSim", LOG_LEVEL_INFO);

    uint32_t nClients = 4;
    uint32_t nServers = 3;
    uint16_t lbPort = 8080;
    uint16_t serverPort = 80;
    double simTime = 10.0;

    NodeContainer clientNodes;
    clientNodes.Create(nClients);
    Ptr<Node> loadBalancerNode = CreateObject<Node>();
    NodeContainer serverNodes;
    serverNodes.Create(nServers);

    InternetStackHelper stack;
    stack.Install(clientNodes);
    stack.Install(loadBalancerNode);
    stack.Install(serverNodes);

    PointToPointHelper p2pClientLB;
    p2pClientLB.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pClientLB.SetChannelAttribute("Delay", StringValue("2ms"));

    Ipv4AddressHelper address;

    std::vector<Ipv4Address> lbClientSideIps;
    for (uint32_t i = 0; i < nClients; ++i) {
        NetDeviceContainer devices = p2pClientLB.Install(clientNodes.Get(i), loadBalancerNode);
        std::ostringstream subnet;
        subnet << "10.1." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        lbClientSideIps.push_back(ifaces.GetAddress(1));
    }

    // ---------------------------------------------------------
    // HETEROGENEOUS DATA CENTER CONFIGURATION
    // ---------------------------------------------------------
    std::vector<Ipv4Address> backendServerIps;
    std::vector<Ptr<Queue<Packet>>> lbQueues; 

    for (uint32_t i = 0; i < nServers; ++i) {
        PointToPointHelper p2pLBServer;
        
        if (i == 0) {
            p2pLBServer.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
            p2pLBServer.SetChannelAttribute("Delay", StringValue("10ms"));
            p2pLBServer.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("20p"));
        } else {
            p2pLBServer.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
            p2pLBServer.SetChannelAttribute("Delay", StringValue("1ms"));
            p2pLBServer.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1000p"));
        }

        NetDeviceContainer devices = p2pLBServer.Install(loadBalancerNode, serverNodes.Get(i));
        
        Ptr<PointToPointNetDevice> ptpDevice = DynamicCast<PointToPointNetDevice>(devices.Get(0));
        lbQueues.push_back(ptpDevice->GetQueue());

        std::ostringstream subnet;
        subnet << "10.2." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        backendServerIps.push_back(ifaces.GetAddress(1));

        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), serverPort));
        ApplicationContainer sinkApp = sinkHelper.Install(serverNodes.Get(i));
        sinkApp.Start(Seconds(1.0));
        sinkApp.Stop(Seconds(simTime));
    }
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---------------------------------------------------------
    // LOAD BALANCER APPLICATION CONFIGURATION
    // ---------------------------------------------------------
    Ptr<LoadBalancerApp> lbApp = CreateObject<LoadBalancerApp>();
    
    // IMPORTANT: Change this parameter to test the different algorithms!
    // Options: LEAST_CONNECTIONS, ROUND_ROBIN, IP_HASH, WEIGHTED_ROUND_ROBIN
    lbApp->Setup(lbPort, backendServerIps, serverPort, WEIGHTED_ROUND_ROBIN); 
    lbApp->SetQueues(lbQueues);
    
    loadBalancerNode->AddApplication(lbApp);
    lbApp->SetStartTime(Seconds(1.0));
    lbApp->SetStopTime(Seconds(simTime));

    lbApp->SetServerProcessingTime(backendServerIps[0], Seconds(5.0)); 
    lbApp->SetServerProcessingTime(backendServerIps[1], Seconds(0.2)); 
    lbApp->SetServerProcessingTime(backendServerIps[2], Seconds(0.2)); 

    lbApp->SetServerWeight(backendServerIps[0], 1); 
    lbApp->SetServerWeight(backendServerIps[1], 3); 
    lbApp->SetServerWeight(backendServerIps[2], 3); 

    Simulator::Schedule(Seconds(4.0), &LoadBalancerApp::SetServerHealth, lbApp, backendServerIps[1], false);

    // ---------------------------------------------------------
    // BURST TRAFFIC GENERATION
    // ---------------------------------------------------------
    for (uint32_t i = 0; i < nClients; ++i) {
        Address lbTargetAddr(InetSocketAddress(lbClientSideIps[i], lbPort));
        OnOffHelper clientOnOff("ns3::UdpSocketFactory", lbTargetAddr);

        clientOnOff.SetAttribute("DataRate", StringValue("5Mbps"));
        clientOnOff.SetAttribute("PacketSize", UintegerValue(1024));

        Ptr<ParetoRandomVariable> onVar = CreateObject<ParetoRandomVariable>();
        onVar->SetAttribute("Scale", DoubleValue(0.2)); 
        onVar->SetAttribute("Shape", DoubleValue(1.5));
        clientOnOff.SetAttribute("OnTime", PointerValue(onVar));

        Ptr<ExponentialRandomVariable> offVar = CreateObject<ExponentialRandomVariable>();
        offVar->SetAttribute("Mean", DoubleValue(0.8));
        clientOnOff.SetAttribute("OffTime", PointerValue(offVar));

        ApplicationContainer clientApp = clientOnOff.Install(clientNodes.Get(i));
        clientApp.Start(Seconds(2.0 + i * 0.15));
        clientApp.Stop(Seconds(9.0));
    }

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    NS_LOG_INFO("Starting Simulation...");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    lbApp->PrintMetricsSummary(simTime);

    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();

    std::cout << "3. AVERAGE RESPONSE TIME & LATENCY (FLOWMONITOR)\n";
    std::cout << "---------------------------------------------------------------------\n";
    for (auto const &item : stats) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(item.first);
        double rxPackets = item.second.rxPackets;
        
        if (rxPackets > 0) {
            double meanDelayMs = (item.second.delaySum.GetSeconds() / rxPackets) * 1000.0;
            std::cout << "Flow " << item.first << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")\n";
            std::cout << "  Mean Latency / Response Delay: " << std::fixed << std::setprecision(3) << meanDelayMs << " ms\n";
        } else {
            std::cout << "Flow " << item.first << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")\n";
            std::cout << "  Mean Latency: N/A (0 packets received)\n";
        }
        
        if (item.second.lostPackets > 0) {
            std::cout << "  *** WARNING: " << item.second.lostPackets << " dropped packets due to congestion! ***\n";
        }
    }
    std::cout << "=====================================================================\n\n";

    monitor->SerializeToXmlFile("load_balancer_flowmon.xml", true, true);
    Simulator::Destroy();
    return 0;
}
