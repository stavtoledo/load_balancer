#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("LoadBalancerTopology");

int main(int argc, char *argv[]) {
    // 0. Enable logging
    LogComponentEnable("LoadBalancerTopology", LOG_LEVEL_INFO);

    uint32_t nClients = 3;
    uint32_t nServers = 3;

    NS_LOG_INFO("Creating Node Containers...");
    
    // 1. Create Nodes
    NodeContainer clientNodes;
    clientNodes.Create(nClients);

    Ptr<Node> loadBalancerNode = CreateObject<Node>();

    NodeContainer serverNodes;
    serverNodes.Create(nServers);

    // 2. Install Internet Stack on all nodes
    InternetStackHelper stack;
    stack.Install(clientNodes);
    stack.Install(loadBalancerNode);
    stack.Install(serverNodes);

    // 3. Configure Point-to-Point Links
    PointToPointHelper p2pClientLB;
    p2pClientLB.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pClientLB.SetChannelAttribute("Delay", StringValue("2ms"));

    PointToPointHelper p2pLBServer;
    p2pLBServer.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pLBServer.SetChannelAttribute("Delay", StringValue("1ms"));

    Ipv4AddressHelper address;

    // 4. Connect Clients to Load Balancer & Assign IPs
    // Subnet pool: 10.1.x.0/24
    std::vector<Ipv4InterfaceContainer> clientInterfaces;
    for (uint32_t i = 0; i < nClients; ++i) {
        NetDeviceContainer devices = p2pClientLB.Install(clientNodes.Get(i), loadBalancerNode);
        
        std::ostringstream subnet;
        subnet << "10.1." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        clientInterfaces.push_back(ifaces);
        
        NS_LOG_INFO("Client " << i << " IP: " << ifaces.GetAddress(0) 
                              << " -> LB Interface IP: " << ifaces.GetAddress(1));
    }

    // 5. Connect Load Balancer to Servers & Assign IPs
    // Subnet pool: 10.2.x.0/24
    std::vector<Ipv4InterfaceContainer> serverInterfaces;
    for (uint32_t i = 0; i < nServers; ++i) {
        NetDeviceContainer devices = p2pLBServer.Install(loadBalancerNode, serverNodes.Get(i));
        
        std::ostringstream subnet;
        subnet << "10.2." << (i + 1) << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        serverInterfaces.push_back(ifaces);
        
        NS_LOG_INFO("LB Interface IP: " << ifaces.GetAddress(0) 
                              << " -> Server " << i << " IP: " << ifaces.GetAddress(1));
    }

    // Populate routing tables so packets know how to hop across interfaces
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    NS_LOG_INFO("Topology setup complete. Starting simulation...");

    Simulator::Stop(Seconds(10.0));
    Simulator::Run();
    Simulator::Destroy();

    return 0;
}
