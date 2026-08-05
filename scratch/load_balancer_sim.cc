#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("LoadBalancerSim");

// ============================================================================
// Phase 3: Custom Load Balancer Application
// ============================================================================
class LoadBalancerApp : public Application {
public:
    LoadBalancerApp() 
        : m_port(8080), m_nextServerIdx(0) {}

    void Setup(uint16_t listenPort, const std::vector<Ipv4Address> &serverIps, uint16_t serverPort) {
        m_port = listenPort;
        m_serverIps = serverIps;
        m_serverPort = serverPort;
    }

private:
    virtual void StartApplication(void) override {
        // Create UDP listening socket on the Load Balancer
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
        m_socket->Bind(local);
        m_socket->SetRecvCallback(MakeCallback(&LoadBalancerApp::HandleRead, this));

        NS_LOG_INFO("[LoadBalancer] Listening on port " << m_port << " with " 
                    << m_serverIps.size() << " backend servers.");
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
            InetSocketAddress clientAddr = InetSocketAddress::ConvertFrom(from);
            NS_LOG_INFO("[LoadBalancer] Received " << packet->GetSize() 
                        << " bytes from Client IP " << clientAddr.GetIpv4() 
                        << ":" << clientAddr.GetPort());

            // Apply Round-Robin Algorithm
            Ipv4Address targetServer = SelectNextServerRoundRobin();

            NS_LOG_INFO("[LoadBalancer] Round-Robin -> Forwarding to Server " 
                        << m_nextServerIdx << " (" << targetServer << ")");

            // Forward request payload to target backend server
            ForwardPacket(packet, targetServer);
        }
    }

    Ipv4Address SelectNextServerRoundRobin() {
        if (m_serverIps.empty()) {
            NS_FATAL_ERROR("No backend servers registered in Load Balancer!");
        }

        Ipv4Address selected = m_serverIps[m_nextServerIdx];
        
        // Increment & wrap around for Round Robin
        m_nextServerIdx = (m_nextServerIdx + 1) % m_serverIps.size();

        return selected;
    }

    void ForwardPacket(Ptr<Packet> packet, Ipv4Address targetServerIp) {
        Ptr<Socket> forwardSocket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        InetSocketAddress serverAddr = InetSocketAddress(targetServerIp, m_serverPort);
        
        forwardSocket->Connect(serverAddr);
        forwardSocket->Send(packet);
        forwardSocket->Close();
    }

    uint16_t m_port;
    Ptr<Socket> m_socket;
    std::vector<Ipv4Address> m_serverIps;
    uint16_t m_serverPort;
    size_t m_nextServerIdx;
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

    // 3. Configure Point-to-Point Helpers
    PointToPointHelper p2pClientLB;
    p2pClientLB.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pClientLB.SetChannelAttribute("Delay", StringValue("2ms"));

    PointToPointHelper p2pLBServer;
    p2pLBServer.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pLBServer.SetChannelAttribute("Delay", StringValue("1ms"));

    Ipv4AddressHelper address;

    // 4. Connect Clients -> Load Balancer
    std::vector<Ipv4Address> lbClientSideIps;
    for (uint32_t i = 0; i < nClients; ++i) {
        NetDeviceContainer devices = p2pClientLB.Install(clientNodes.Get(i), loadBalancerNode);
        
        std::ostringstream subnet;
        subnet << "10.1." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        lbClientSideIps.push_back(ifaces.GetAddress(1)); // LB interface IP on this link
    }

    // 5. Connect Load Balancer -> Servers
    std::vector<Ipv4Address> backendServerIps;
    for (uint32_t i = 0; i < nServers; ++i) {
        NetDeviceContainer devices = p2pLBServer.Install(loadBalancerNode, serverNodes.Get(i));
        
        std::ostringstream subnet;
        subnet << "10.2." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        backendServerIps.push_back(ifaces.GetAddress(1)); // Real Server IP
        
        // Install PacketSink on each server to receive HTTP-like packets
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), serverPort));
        ApplicationContainer sinkApp = sinkHelper.Install(serverNodes.Get(i));
        sinkApp.Start(Seconds(1.0));
        sinkApp.Stop(Seconds(10.0));
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // 6. Install Load Balancer App on Load Balancer Node
    Ptr<LoadBalancerApp> lbApp = CreateObject<LoadBalancerApp>();
    lbApp->Setup(lbPort, backendServerIps, serverPort);
    loadBalancerNode->AddApplication(lbApp);
    lbApp->SetStartTime(Seconds(1.0));
    lbApp->SetStopTime(Seconds(10.0));

    // 7. Install OnOffTraffic Applications on Clients pointing to Load Balancer
    for (uint32_t i = 0; i < nClients; ++i) {
        // Send traffic to the LB interface IP associated with this client
        Address lbTargetAddr(InetSocketAddress(lbClientSideIps[i], lbPort));
        
        OnOffHelper clientOnOff("ns3::UdpSocketFactory", lbTargetAddr);
        clientOnOff.SetAttribute("DataRate", StringValue("1Mbps"));
        clientOnOff.SetAttribute("PacketSize", UintegerValue(512));
        clientOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=0.1]"));
        clientOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0.9]"));

        ApplicationContainer clientApp = clientOnOff.Install(clientNodes.Get(i));
        clientApp.Start(Seconds(2.0 + i * 0.5)); // Stagger client requests
        clientApp.Stop(Seconds(9.0));
    }

    NS_LOG_INFO("Starting Load Balancer Simulation...");
    Simulator::Stop(Seconds(10.0));
    Simulator::Run();
    Simulator::Destroy();

    return 0;
}
