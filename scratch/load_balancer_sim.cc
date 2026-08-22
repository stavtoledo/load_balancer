#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include <map>
#include <limits>
#include <iostream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("LoadBalancerSim");

enum LoadBalancingStrategy {
    ROUND_ROBIN,
    LEAST_CONNECTIONS
};

// ============================================================================
// Phase 3 & 4: Load Balancer Application
// ============================================================================
class LoadBalancerApp : public Application {
public:
    LoadBalancerApp() 
        : m_port(8080), 
          m_serverPort(80), 
          m_nextServerIdx(0), 
          m_strategy(LEAST_CONNECTIONS),
          m_sessionDuration(Seconds(1.5)) {}

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
        }
    }

    void SetServerHealth(Ipv4Address serverIp, bool isHealthy) {
        m_serverHealth[serverIp] = isHealthy;
        if (!isHealthy) {
            NS_LOG_INFO("[HealthCheck] *** SERVER FAILURE DETECTED *** Server " << serverIp << " is DOWN!");
            std::cout << "\n========================================\n"
                      << "[HealthCheck] SERVER " << serverIp << " IS NOW DOWN!\n"
                      << "========================================\n" << std::endl;
            m_activeConnections[serverIp] = 0;
        } else {
            NS_LOG_INFO("[HealthCheck] Server " << serverIp << " recovered and is UP!");
        }
    }

private:
    virtual void StartApplication(void) override {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
        m_socket->Bind(local);
        m_socket->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleRead, this));

        NS_LOG_INFO("[LoadBalancer] Listening on port " << m_port);
    }

    virtual void StopApplication(void) override {
        if (m_socket) {
            m_socket->Close();
        }
    }

    void HandleRead(Ptr<Socket> socket) {
        Ptr<Packet> packet;
        Address from;

        while ((packet = socket->RecvFrom(from))) {
            Ipv4Address targetServer = SelectBackendServer();

            if (targetServer == Ipv4Address("0.0.0.0")) {
                NS_LOG_ERROR("[LoadBalancer] All backend servers are DOWN!");
                continue;
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
};

// ============================================================================
// Topology & Simulation Runner with FlowMonitor
// ============================================================================
int main(int argc, char *argv[]) {
    LogComponentEnable("LoadBalancerSim", LOG_LEVEL_INFO);

    uint32_t nClients = 4;
    uint32_t nServers = 3;
    uint16_t lbPort = 8080;
    uint16_t serverPort = 80;

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

    // 3. Point-to-Point Configuration
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
        sinkApp.Stop(Seconds(10.0));
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // 6. Install Load Balancer App
    Ptr<LoadBalancerApp> lbApp = CreateObject<LoadBalancerApp>();
    lbApp->Setup(lbPort, backendServerIps, serverPort, LEAST_CONNECTIONS);
    loadBalancerNode->AddApplication(lbApp);
    lbApp->SetStartTime(Seconds(1.0));
    lbApp->SetStopTime(Seconds(10.0));

    // Simulate Server Failure at t = 4.0s
    Simulator::Schedule(Seconds(4.0), &LoadBalancerApp::SetServerHealth, lbApp, backendServerIps[1], false);

    // 7. Install Client Traffic Generators
    for (uint32_t i = 0; i < nClients; ++i) {
        Address lbTargetAddr(InetSocketAddress(lbClientSideIps[i], lbPort));
        OnOffHelper clientOnOff("ns3::UdpSocketFactory", lbTargetAddr);
        clientOnOff.SetAttribute("DataRate", StringValue("2Mbps"));
        clientOnOff.SetAttribute("PacketSize", UintegerValue(1024));
        clientOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=0.3]"));
        clientOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0.7]"));

        ApplicationContainer clientApp = clientOnOff.Install(clientNodes.Get(i));
        clientApp.Start(Seconds(2.0 + i * 0.2));
        clientApp.Stop(Seconds(9.0));
    }

    // ========================================================================
    // PHASE 5: FlowMonitor Setup
    // ========================================================================
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    NS_LOG_INFO("Starting Simulation with FlowMonitor...");
    Simulator::Stop(Seconds(10.0));
    Simulator::Run();

    // Check stats right before destroying simulator
    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();

    std::cout << "\n=====================================================================\n";
    std::cout << "                  PHASE 5: PERFORMANCE EVALUATION SUMMARY            \n";
    std::cout << "=====================================================================\n";

    for (auto const &item : stats) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(item.first);

        double txPackets = item.second.txPackets;
        double rxPackets = item.second.rxPackets;
        double lostPackets = item.second.lostPackets;
        double packetLossRate = (txPackets > 0) ? ((txPackets - rxPackets) / txPackets) * 100.0 : 0.0;

        // Calculate Throughput in Kbps over active simulation window (7 seconds)
        double throughputKbps = (item.second.rxBytes * 8.0) / (7.0 * 1000.0);

        // Calculate Mean Delay in milliseconds
        double meanDelayMs = (rxPackets > 0) ? (item.second.delaySum.GetSeconds() / rxPackets) * 1000.0 : 0.0;

        std::cout << "Flow " << item.first << " (" << t.sourceAddress << ":" << t.sourcePort 
                  << " -> " << t.destinationAddress << ":" << t.destinationPort << ")\n";
        std::cout << "  Tx Packets : " << txPackets << "\n";
        std::cout << "  Rx Packets : " << rxPackets << "\n";
        std::cout << "  Packet Loss: " << lostPackets << " (" << packetLossRate << "%)\n";
        std::cout << "  Throughput : " << throughputKbps << " Kbps\n";
        std::cout << "  Mean Delay : " << meanDelayMs << " ms\n";
        std::cout << "---------------------------------------------------------------------\n";
    }

    // Save full XML trace report for parsing
    monitor->SerializeToXmlFile("load_balancer_flowmon.xml", true, true);
    std::cout << "\n[FlowMonitor] Complete XML report saved to 'load_balancer_flowmon.xml'\n\n";

    Simulator::Destroy();
    return 0;
}
