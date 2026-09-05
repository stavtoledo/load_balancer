#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include <map>
#include <limits>
#include <iostream>
#include <iomanip>
#include <string>
#include <fstream> // Added for CSV logging

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("LoadBalancerSim");

enum LoadBalancingStrategy {
    ROUND_ROBIN,
    LEAST_CONNECTIONS
};

// ============================================================================
// Phase 3, 4 & 5: Custom Load Balancer Application
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
          m_requestsThisInterval(0) {}

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
        }
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
            
            // Calculate final busy time to ensure active servers don't show 0%
            Time finalBusyTime = m_serverBusyTime[ip];
            if (m_activeConnections[ip] > 0 && m_serverHealth[ip]) {
                finalBusyTime += (Simulator::Now() - m_lastBusyStart[ip]);
            }

            double util = (finalBusyTime.GetSeconds() / totalSimTime) * 100.0;
            if (util > 100.0) util = 100.0;

            // FIX: Convert IP to string before passing to setw()
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

        // Initialize CSV Logger for traffic plotting
        m_csv.open("traffic_pattern.csv");
        m_csv << "Time,Requests\n";
        Simulator::Schedule(Seconds(0.1), &LoadBalancerApp::LogTraffic, this);
    }

    virtual void StopApplication(void) override {
        if (m_socket) {
            m_socket->Close();
        }
        if (m_csv.is_open()) {
            m_csv.close();
        }
    }

    void LogTraffic() {
        m_csv << Simulator::Now().GetSeconds() << "," << m_requestsThisInterval << "\n";
        m_requestsThisInterval = 0; 
        Simulator::Schedule(Seconds(0.1), &LoadBalancerApp::LogTraffic, this);
    }

    void HandleRead(Ptr<Socket> socket) {
        Ptr<Packet> packet;
        Address from;

        while ((packet = socket->RecvFrom(from))) {
            m_totalRequests++;
            m_requestsThisInterval++; // Log for CSV

            Ipv4Address targetServer = SelectBackendServer();

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
            Simulator::Schedule(m_sessionDuration, &LoadBalancerApp::ReleaseConnection, this, targetServer);
        }
    }

    Ipv4Address SelectBackendServer() {
        if (m_strategy == LEAST_CONNECTIONS) {
            return SelectLeastConnections();
        } else {
            return SelectRoundRobin();
        }
    }

    Ipv4Address SelectRoundRobin() {
        size_t attempts = 0;
        while (attempts < m_serverIps.size()) {
            Ipv4Address candidate = m_serverIps[m_nextServerIdx];
            m_nextServerIdx = (m_nextServerIdx + 1) % m_serverIps.size();

            if (m_serverHealth[candidate]) {
                return candidate;
            }
            attempts++;
        }
        return Ipv4Address("0.0.0.0");
    }

    Ipv4Address SelectLeastConnections() {
        Ipv4Address selectedServer = Ipv4Address("0.0.0.0");
        uint32_t minConnections = std::numeric_limits<uint32_t>::max();

        for (const auto &ip : m_serverIps) {
            if (!m_serverHealth[ip]) continue;

            uint32_t currentConns = m_activeConnections[ip];
            if (currentConns < minConnections) {
                minConnections = currentConns;
                selectedServer = ip;
            }
        }
        return selectedServer;
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
    Time m_sessionDuration;

    uint32_t m_totalRequests;
    uint32_t m_droppedRequests;
    std::map<Ipv4Address, uint32_t> m_serverRequestCount;
    std::map<Ipv4Address, Time> m_serverBusyTime;
    std::map<Ipv4Address, Time> m_lastBusyStart;

    // CSV Logger variables
    uint32_t m_requestsThisInterval;
    std::ofstream m_csv;
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

    // 1. Create Nodes
    NodeContainer clientNodes;
    clientNodes.Create(nClients);

    Ptr<Node> loadBalancerNode = CreateObject<Node>();

    NodeContainer serverNodes;
    serverNodes.Create(nServers);

    // 2. Install Internet Stack
    InternetStackHelper stack;
    stack.Install(clientNodes);
    stack.Install(loadBalancerNode);
    stack.Install(serverNodes);

    // 3. Point-to-Point Helper Configuration
    PointToPointHelper p2pClientLB;
    p2pClientLB.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pClientLB.SetChannelAttribute("Delay", StringValue("2ms"));

    PointToPointHelper p2pLBServer;
    p2pLBServer.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pLBServer.SetChannelAttribute("Delay", StringValue("1ms"));

    Ipv4AddressHelper address;

    // 4. Client Interfaces
    std::vector<Ipv4Address> lbClientSideIps;
    for (uint32_t i = 0; i < nClients; ++i) {
        NetDeviceContainer devices = p2pClientLB.Install(clientNodes.Get(i), loadBalancerNode);
        std::ostringstream subnet;
        subnet << "10.1." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        lbClientSideIps.push_back(ifaces.GetAddress(1));
    }

    // 5. Server Interfaces
    std::vector<Ipv4Address> backendServerIps;
    for (uint32_t i = 0; i < nServers; ++i) {
        NetDeviceContainer devices = p2pLBServer.Install(loadBalancerNode, serverNodes.Get(i));
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

    // 6. Install Load Balancer App
    Ptr<LoadBalancerApp> lbApp = CreateObject<LoadBalancerApp>();
    lbApp->Setup(lbPort, backendServerIps, serverPort, LEAST_CONNECTIONS);
    loadBalancerNode->AddApplication(lbApp);
    lbApp->SetStartTime(Seconds(1.0));
    lbApp->SetStopTime(Seconds(simTime));

    // Simulate Server Failure at t = 4.0s (Server 1 / 10.2.2.2 fails)
    Simulator::Schedule(Seconds(4.0), &LoadBalancerApp::SetServerHealth, lbApp, backendServerIps[1], false);

    // 7. Configurable Traffic Profile
    std::string trafficProfile = "BURST"; 
    NS_LOG_INFO("Configuring Client Traffic Generators with Profile: " << trafficProfile);

    for (uint32_t i = 0; i < nClients; ++i) {
        Address lbTargetAddr(InetSocketAddress(lbClientSideIps[i], lbPort));
        OnOffHelper clientOnOff("ns3::UdpSocketFactory", lbTargetAddr);

        if (trafficProfile == "STEADY") {
            clientOnOff.SetAttribute("DataRate", StringValue("1.5Mbps"));
            clientOnOff.SetAttribute("PacketSize", UintegerValue(512));

            Ptr<ExponentialRandomVariable> onVar = CreateObject<ExponentialRandomVariable>();
            onVar->SetAttribute("Mean", DoubleValue(0.5));
            clientOnOff.SetAttribute("OnTime", PointerValue(onVar));

            Ptr<ExponentialRandomVariable> offVar = CreateObject<ExponentialRandomVariable>();
            offVar->SetAttribute("Mean", DoubleValue(0.5));
            clientOnOff.SetAttribute("OffTime", PointerValue(offVar));

        } else if (trafficProfile == "BURST") {
            clientOnOff.SetAttribute("DataRate", StringValue("5Mbps"));
            clientOnOff.SetAttribute("PacketSize", UintegerValue(1024));

            Ptr<ParetoRandomVariable> onVar = CreateObject<ParetoRandomVariable>();
            onVar->SetAttribute("Scale", DoubleValue(0.2)); 
            onVar->SetAttribute("Shape", DoubleValue(1.5));
            clientOnOff.SetAttribute("OnTime", PointerValue(onVar));

            Ptr<ExponentialRandomVariable> offVar = CreateObject<ExponentialRandomVariable>();
            offVar->SetAttribute("Mean", DoubleValue(0.8));
            clientOnOff.SetAttribute("OffTime", PointerValue(offVar));

        } else if (trafficProfile == "RANDOM") {
            clientOnOff.SetAttribute("DataRate", StringValue("2Mbps"));
            clientOnOff.SetAttribute("PacketSize", UintegerValue(768));

            Ptr<UniformRandomVariable> onVar = CreateObject<UniformRandomVariable>();
            onVar->SetAttribute("Min", DoubleValue(0.1));
            onVar->SetAttribute("Max", DoubleValue(0.6));
            clientOnOff.SetAttribute("OnTime", PointerValue(onVar));

            Ptr<UniformRandomVariable> offVar = CreateObject<UniformRandomVariable>();
            offVar->SetAttribute("Min", DoubleValue(0.2));
            offVar->SetAttribute("Max", DoubleValue(0.9));
            clientOnOff.SetAttribute("OffTime", PointerValue(offVar));
        }

        ApplicationContainer clientApp = clientOnOff.Install(clientNodes.Get(i));
        clientApp.Start(Seconds(2.0 + i * 0.15));
        clientApp.Stop(Seconds(9.0));
    }

    // 8. FlowMonitor Setup
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    NS_LOG_INFO("Starting Simulation...");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // Output Load Balancer Custom Metrics
    lbApp->PrintMetricsSummary(simTime);

    // Output Network-Layer Flow Statistics
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();

    std::cout << "3. AVERAGE RESPONSE TIME & LATENCY (FLOWMONITOR)\n";
    std::cout << "---------------------------------------------------------------------\n";
    for (auto const &item : stats) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(item.first);
        double rxPackets = item.second.rxPackets;
        double meanDelayMs = (rxPackets > 0) ? (item.second.delaySum.GetSeconds() / rxPackets) * 1000.0 : 0.0;

        std::cout << "Flow " << item.first << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")\n";
        std::cout << "  Mean Latency / Response Delay: " << std::fixed << std::setprecision(3) << meanDelayMs << " ms\n";
    }
    std::cout << "=====================================================================\n\n";

    // Save trace XML file
    monitor->SerializeToXmlFile("load_balancer_flowmon.xml", true, true);

    Simulator::Destroy();
    return 0;
}
