/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2009 IITP RAS
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Based on 
 *      NS-2 AODV model developed by the CMU/MONARCH group and optimized and
 *      tuned by Samir Das and Mahesh Marina, University of Cincinnati;
 * 
 *      AODV-UU implementation by Erik Nordström of Uppsala University
 *      http://core.it.uu.se/core/index.php/AODV-UU
 *
 * Authors: Elena Borovkova <borovkovaes@iitp.ru>
 *          Pavel Boyko <boyko@iitp.ru>
 */
#include "aodv-routing-protocol.h"
#include "ns3/log.h"
#include "ns3/random-variable.h"
#include "ns3/inet-socket-address.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/wifi-net-device.h"
#include "ns3/adhoc-wifi-mac.h"
#include <algorithm>

NS_LOG_COMPONENT_DEFINE ("AodvRoutingProtocol");

namespace ns3
{
namespace aodv
{
NS_OBJECT_ENSURE_REGISTERED (RoutingProtocol);

/// UDP Port for AODV control traffic
const uint32_t RoutingProtocol::AODV_PORT = 654;

RoutingProtocol::RoutingProtocol () :
  RreqRetries (2),
  RreqRateLimit (10),
  ActiveRouteTimeout (Seconds (3)),
  NetDiameter (35),
  NodeTraversalTime (MilliSeconds (40)),
  NetTraversalTime (Scalar (2 * NetDiameter) * NodeTraversalTime),
  PathDiscoveryTime ( Scalar (2) * NetTraversalTime),
  MyRouteTimeout (Scalar (2) * std::max (PathDiscoveryTime, ActiveRouteTimeout)),
  HelloInterval(Seconds (1)),
  AllowedHelloLoss (2),
  DeletePeriod (Scalar(5) * std::max(ActiveRouteTimeout, HelloInterval)),
  NextHopWait (NodeTraversalTime + MilliSeconds (10)),
  TimeoutBuffer (2),
  BlackListTimeout(Scalar (RreqRetries) * NetTraversalTime),
  MaxQueueLen (64),
  MaxQueueTime (Seconds(30)),
  DestinationOnly (false),
  GratuitousReply (true),
  EnableHello (true),
  m_routingTable (DeletePeriod),
  m_queue (MaxQueueLen, MaxQueueTime),
  m_requestId (0),
  m_seqNo (0),
  m_nb(HelloInterval),
  m_rreqCount (0),
  htimer (Timer::CANCEL_ON_DESTROY),
  m_rreqRateLimitTimer (Timer::CANCEL_ON_DESTROY)
{
  if (EnableHello)
    {
      m_nb.SetCallback (MakeCallback (&RoutingProtocol::SendRerrWhenBreaksLinkToNextHop, this));
    }
}

TypeId
RoutingProtocol::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::aodv::RoutingProtocol")
      .SetParent<Ipv4RoutingProtocol> ()
      .AddConstructor<RoutingProtocol> ()
      .AddAttribute ("HelloInterval", "HELLO messages emission interval.",
                     TimeValue (Seconds (1)),
                     MakeTimeAccessor (&RoutingProtocol::HelloInterval),
                     MakeTimeChecker ())
      .AddAttribute ("RreqRetries", "Maximum number of retransmissions of RREQ to discover a route",
                     UintegerValue (2),
                     MakeUintegerAccessor (&RoutingProtocol::RreqRetries),
                     MakeUintegerChecker<uint32_t> ())
      .AddAttribute ("RreqRateLimit", "Maximum number of RREQ per second.",
                     UintegerValue (10),
                     MakeUintegerAccessor (&RoutingProtocol::RreqRateLimit),
                     MakeUintegerChecker<uint32_t> ())
      .AddAttribute ("NodeTraversalTime", "Conservative estimate of the average one hop traversal time for packets and should include "
                     "queuing delays, interrupt processing times and transfer times.",
                     TimeValue (MilliSeconds (40)),
                     MakeTimeAccessor (&RoutingProtocol::NodeTraversalTime),
                     MakeTimeChecker ())
      .AddAttribute ("NextHopWait", "Period of our waiting for the neighbour's RREP_ACK = 10 ms + NodeTraversalTime",
                     TimeValue (MilliSeconds (50)),
                     MakeTimeAccessor (&RoutingProtocol::NextHopWait),
                     MakeTimeChecker ())
      .AddAttribute ("ActiveRouteTimeout", "Period of time during which the route is considered to be valid",
                     TimeValue (Seconds (3)),
                     MakeTimeAccessor (&RoutingProtocol::ActiveRouteTimeout),
                     MakeTimeChecker ())
      .AddAttribute ("MyRouteTimeout", "Value of lifetime field in RREP generating by this node = 2 * max(ActiveRouteTimeout, PathDiscoveryTime)",
                     TimeValue (Seconds (11.2)),
                     MakeTimeAccessor (&RoutingProtocol::MyRouteTimeout),
                     MakeTimeChecker ())
      .AddAttribute ("BlackListTimeout", "Time for which the node is put into the blacklist = RreqRetries * NetTraversalTime",
                     TimeValue (Seconds (5.6)),
                     MakeTimeAccessor (&RoutingProtocol::BlackListTimeout),
                     MakeTimeChecker ())
      .AddAttribute ("DeletePeriod", "DeletePeriod is intended to provide an upper bound on the time for which an upstream node A "
                     "can have a neighbor B as an active next hop for destination D, while B has invalidated the route to D."
                     " = 5 * max (HelloInterval, ActiveRouteTimeout)",
                     TimeValue (Seconds (15)),
                     MakeTimeAccessor (&RoutingProtocol::DeletePeriod),
                     MakeTimeChecker ())
      .AddAttribute ("TimeoutBuffer", "Its purpose is to provide a buffer for the timeout so that if the RREP is delayed"
                     " due to congestion, a timeout is less likely to occur while the RREP is still en route back to the source.",
                     UintegerValue (2),
                     MakeUintegerAccessor (&RoutingProtocol::TimeoutBuffer),
                     MakeUintegerChecker<uint16_t> ())
      .AddAttribute ("NetDiameter", "Net diameter measures the maximum possible number of hops between two nodes in the network",
                     UintegerValue (35),
                     MakeUintegerAccessor (&RoutingProtocol::NetDiameter),
                     MakeUintegerChecker<uint32_t> ())
      .AddAttribute ("NetTraversalTime", "Estimate of the average net traversal time = 2 * NodeTraversalTime * NetDiameter",
                     TimeValue (Seconds (2.8)),
                     MakeTimeAccessor (&RoutingProtocol::NetTraversalTime),
                     MakeTimeChecker ())
      .AddAttribute ("PathDiscoveryTime", "Estimate of maximum time needed to find route in network = 2 * NetTraversalTime",
                     TimeValue (Seconds (5.6)),
                     MakeTimeAccessor (&RoutingProtocol::PathDiscoveryTime),
                     MakeTimeChecker ())
      .AddAttribute ("MaxQueueLen", "Maximum number of packets that we allow a routing protocol to buffer.",
                     UintegerValue (64),
                     MakeUintegerAccessor (&RoutingProtocol::MaxQueueLen),
                     MakeUintegerChecker<uint32_t> ())
      .AddAttribute ("MaxQueueTime", "Maximum time packets can be queued (in seconds)",
                     TimeValue (Seconds (30)),
                     MakeTimeAccessor (&RoutingProtocol::MaxQueueTime),
                     MakeTimeChecker ())
      .AddAttribute ("AllowedHelloLoss", "Number of hello messages which may be loss for valid link.",
                     UintegerValue (2),
                     MakeUintegerAccessor (&RoutingProtocol::AllowedHelloLoss),
                     MakeUintegerChecker<uint16_t> ())
      .AddAttribute ("GratuitousReply", "Indicates whether a gratuitous RREP should be unicast to the node originated route discovery.",
                     BooleanValue (true),
                     MakeBooleanAccessor (&RoutingProtocol::SetGratuitousReplyFlag,
                                          &RoutingProtocol::GetGratuitousReplyFlag),
                     MakeBooleanChecker ())
      .AddAttribute ("DestinationOnly", "Indicates only the destination may respond to this RREQ.",
                     BooleanValue (false),
                     MakeBooleanAccessor (&RoutingProtocol::SetDesinationOnlyFlag,
                                          &RoutingProtocol::GetDesinationOnlyFlag),
                     MakeBooleanChecker ())
      .AddAttribute ("EnableHello", "Indicates whether a hello messages enable.",
                     BooleanValue (true),
                     MakeBooleanAccessor (&RoutingProtocol::SetHelloEnable,
                                          &RoutingProtocol::GetHelloEnable),
                     MakeBooleanChecker ())
  ;
  return tid;
}

RoutingProtocol::~RoutingProtocol ()
{
}

void
RoutingProtocol::DoDispose ()
{
  m_ipv4 = 0;
  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::iterator iter =
      m_socketAddresses.begin (); iter != m_socketAddresses.end (); iter++)
    {
      iter->first->Close ();
    }
  m_socketAddresses.clear ();
  Ipv4RoutingProtocol::DoDispose ();
}

void
RoutingProtocol::Start ()
{
  m_scb = MakeCallback (&RoutingProtocol::Send, this);
  m_ecb = MakeCallback (&RoutingProtocol::Drop, this);

  if (EnableHello)
    {
      m_nb.ScheduleTimer ();
    }
  m_rreqRateLimitTimer.SetFunction (&RoutingProtocol::RreqRateLimitTimerExpire,
      this);
  m_rreqRateLimitTimer.Schedule (Seconds (1));
}

Ptr<Ipv4Route>
RoutingProtocol::RouteOutput (Ptr<Packet> p, const Ipv4Header &header,
    uint32_t oif, Socket::SocketErrno &sockerr)
{
  NS_LOG_FUNCTION (this << header.GetDestination ());
  if (m_socketAddresses.empty ())
    {
      sockerr = Socket::ERROR_NOROUTETOHOST;
      NS_LOG_LOGIC ("No aodv interfaces");
      Ptr<Ipv4Route> route;
      return route;
    }
  sockerr = Socket::ERROR_NOTERROR;
  Ptr<Ipv4Route> route;
  Ipv4Address dst = header.GetDestination ();
  RoutingTableEntry rt;
  if (m_routingTable.LookupRoute (dst, rt))
    {
      if (rt.GetFlag () == VALID)
        {
          route = rt.GetRoute ();
          NS_ASSERT (route != 0);
          NS_LOG_LOGIC("exist route to " << route->GetDestination() << " from interface " << route->GetSource());
          UpdateRouteLifeTime (dst, ActiveRouteTimeout);
          UpdateRouteLifeTime (route->GetGateway (), ActiveRouteTimeout);
        }
      else
        {
          bool result = true;
          // May be null pointer (e.g. tcp-socket give null pointer)
          if (p != Ptr<Packet> ())
            {
              QueueEntry newEntry (p, header, m_scb, m_ecb);
              result = m_queue.Enqueue (newEntry);
              if (result)
                NS_LOG_LOGIC ("Add packet " << p->GetUid() << " to queue");

            }
          if ((rt.GetFlag () == INVALID) && result)
            {
              SendRequest (dst);
            }
        }
    }
  else
    {
      bool result = true;
      if (p != Ptr<Packet> ())
        {
          QueueEntry newEntry (p, header, m_scb, m_ecb);
          // Some protocols may ask route several times for a single packet.
          result = m_queue.Enqueue (newEntry);
          if (result)
            NS_LOG_LOGIC ("Add packet " << p->GetUid() << " to queue. Protocol " << (uint16_t) header.GetProtocol ());
        }
      if (result)
        SendRequest (dst);
    }
  return route;
}

bool
RoutingProtocol::RouteInput (Ptr<const Packet> p, const Ipv4Header &header,
    Ptr<const NetDevice> idev, UnicastForwardCallback ucb,
    MulticastForwardCallback mcb, LocalDeliverCallback lcb, ErrorCallback ecb)
{
  NS_LOG_FUNCTION (this << p->GetUid() << header.GetDestination() << idev->GetAddress());
  if (m_socketAddresses.empty ())
    {
      NS_LOG_LOGIC ("No aodv interfaces");
      return false;
    }
  NS_ASSERT (m_ipv4 != 0);
  // Check if input device supports IP
  NS_ASSERT (m_ipv4->GetInterfaceForDevice (idev) >= 0);
  int32_t iif = m_ipv4->GetInterfaceForDevice (idev);

  Ipv4Address dst = header.GetDestination ();
  Ipv4Address origin = header.GetSource ();

  if (IsMyOwnAddress (origin))
    return true;

  // Local delivery to AODV interfaces
  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator j =
      m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ipv4InterfaceAddress iface = j->second;
      if (m_ipv4->GetInterfaceForAddress (iface.GetLocal ()) == iif)
        if (dst == iface.GetBroadcast ())
          {
            if (m_idCache.LookupId (origin, p->GetUid ()))
              {
                NS_LOG_DEBUG ("Duplicated packet " << p->GetUid () << " from " << origin << ". Drop.");
                return true;
              }
            m_idCache.InsertId (origin, p->GetUid (), PathDiscoveryTime);
            UpdateRouteLifeTime (origin, ActiveRouteTimeout);
            NS_LOG_LOGIC ("Broadcast local delivery to " << iface.GetLocal ());
            Ptr<Packet> packet = p->Copy ();
            lcb (p, header, iif);
            if (header.GetTtl () > 1)
              {
                NS_LOG_LOGIC ("Forward broadcast. TTL " << (uint16_t) header.GetTtl ());
                RoutingTableEntry toBroadcast;
                if (m_routingTable.LookupRoute (dst, toBroadcast))
                  {
                    Ptr<Ipv4Route> route = toBroadcast.GetRoute ();
                    ucb (route, packet, header);
                  }
                else
                  {
                    NS_LOG_DEBUG ("No route to forward broadcast. Drop packet " << p->GetUid ());
                  }
              }
            else
              {
                NS_LOG_DEBUG ("TTL exceeded. Drop packet " << p->GetUid ());
              }
            return true;
          }
    }
  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator j =
      m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ipv4InterfaceAddress iface = j->second;
      if (dst == iface.GetLocal ())
        {
          UpdateRouteLifeTime (origin, ActiveRouteTimeout);
          RoutingTableEntry toOrigin;
          if (m_routingTable.LookupRoute (origin, toOrigin))
            {
              UpdateRouteLifeTime (toOrigin.GetNextHop (), ActiveRouteTimeout);
              m_nb.Update (toOrigin.GetNextHop (), ActiveRouteTimeout);
            }
          NS_LOG_LOGIC ("Unicast local delivery to " << iface.GetLocal ());
          lcb (p, header, iif);
          return true;
        }
    }

  // TODO: local delivery to non-AODV interfaces
  // Forwarding
  return Forwarding (p, header, ucb, ecb);
}

bool
RoutingProtocol::Forwarding (Ptr<const Packet> p, const Ipv4Header & header,
    UnicastForwardCallback ucb, ErrorCallback ecb)
{
  Ipv4Address dst = header.GetDestination ();
  Ipv4Address origin = header.GetSource ();
  m_routingTable.Purge ();
  RoutingTableEntry toDst;
  if (m_routingTable.LookupRoute (dst, toDst))
    {
      if (toDst.GetFlag () == VALID)
        {
          Ptr<Ipv4Route> route = toDst.GetRoute ();
          NS_LOG_LOGIC (route->GetSource()<<" forwarding to " << dst << " from " << origin << " packet " << p->GetUid ());

          /*
           *  Each time a route is used to forward a data packet, its Active Route
           *  Lifetime field of the source, destination and the next hop on the
           *  path to the destination is updated to be no less than the current
           *  time plus ActiveRouteTimeout.
           */
          UpdateRouteLifeTime (origin, ActiveRouteTimeout);
          UpdateRouteLifeTime (dst, ActiveRouteTimeout);
          UpdateRouteLifeTime (route->GetGateway (), ActiveRouteTimeout);
          /*
           *  Since the route between each originator and destination pair is expected to be symmetric, the
           *  Active Route Lifetime for the previous hop, along the reverse path back to the IP source, is also updated
           *  to be no less than the current time plus ActiveRouteTimeout
           */
          RoutingTableEntry toOrigin;
          m_routingTable.LookupRoute (origin, toOrigin);
          UpdateRouteLifeTime (toOrigin.GetNextHop (), ActiveRouteTimeout);

          m_nb.Update (route->GetGateway (), ActiveRouteTimeout);
          m_nb.Update (toOrigin.GetNextHop (), ActiveRouteTimeout);

          ucb (route, p, header);
          return true;
        }
      else
        {
          if (toDst.GetValidSeqNo ())
            {
              SendRerrWhenNoRouteToForward (dst, toDst.GetSeqNo (), origin);
              NS_LOG_DEBUG ("Drop packet " << p->GetUid () << " because no route to forward it.");
              return false;
            }
        }
    }
  NS_LOG_LOGIC ("route not found to "<< dst << ". Send RERR message.");
  NS_LOG_DEBUG ("Drop packet " << p->GetUid () << " because no route to forward it.");
  SendRerrWhenNoRouteToForward (dst, 0, origin);
  return false;
}

void
RoutingProtocol::SetIpv4 (Ptr<Ipv4> ipv4)
{
  NS_ASSERT (ipv4 != 0);
  NS_ASSERT (m_ipv4 == 0);

  if (EnableHello)
    {
      htimer.SetFunction (&RoutingProtocol::HelloTimerExpire, this);
      htimer.Schedule (MilliSeconds (UniformVariable ().GetValue (0.0, 100.0)));
    }

  m_ipv4 = ipv4;
  Simulator::ScheduleNow (&RoutingProtocol::Start, this);
}

void
RoutingProtocol::NotifyInterfaceUp (uint32_t i)
{
  NS_LOG_FUNCTION (this << m_ipv4->GetAddress (i, 0).GetLocal ());
  Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
  if (l3->GetNAddresses (i) > 1)
    {
      NS_LOG_LOGIC ("AODV does not work with more then one address per each interface.");
    }
  Ipv4InterfaceAddress iface = l3->GetAddress (i, 0);
  if (iface.GetLocal () == Ipv4Address ("127.0.0.1"))
    return;
  
  // Create a socket to listen only on this interface
  Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
      UdpSocketFactory::GetTypeId ());
  NS_ASSERT (socket != 0);
  socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvAodv, this));
  socket->Bind (InetSocketAddress (iface.GetLocal (), AODV_PORT));
  socket->Connect (InetSocketAddress (iface.GetBroadcast (), AODV_PORT));
  socket->SetAttribute ("IpTtl", UintegerValue (1));
  m_socketAddresses.insert (std::make_pair (socket, iface));

  // Add local broadcast record to the routing table
  Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (iface.GetLocal ()));
  RoutingTableEntry rt (/*device=*/dev, /*dst=*/iface.GetBroadcast (), /*know seqno=*/true, /*seqno=*/0, /*iface=*/iface,
                        /*hops=*/1, /*next hop=*/iface.GetBroadcast (), /*lifetime=*/Seconds (1e9)); // TODO use infty
  m_routingTable.AddRoute (rt);
  
  // Allow neighbor manager use this interface for layer 2 feedback if possible
  Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice> ();
  if (wifi == 0)
    return;
  Ptr<WifiMac> mac = wifi->GetMac ()->GetObject<AdhocWifiMac> ();
  if (mac == 0)
    return;
  
  mac->TraceConnectWithoutContext ("TxErrHeader", m_nb.GetTxErrorCallback ());
  m_nb.AddArpCache (l3->GetInterface (i)->GetArpCache ());
}

void
RoutingProtocol::NotifyInterfaceDown (uint32_t i)
{
  NS_LOG_FUNCTION (this << m_ipv4->GetAddress (i, 0).GetLocal ());

  // Disable layer 2 link state monitoring (if possible)
  Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
  Ptr<NetDevice> dev = l3->GetNetDevice (i);
  Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice> ();
  if (wifi != 0)
    {
      Ptr<WifiMac> mac = wifi->GetMac ()->GetObject<AdhocWifiMac> ();
      if (mac != 0)
        {
          mac->TraceDisconnectWithoutContext ("TxErrHeader",
              m_nb.GetTxErrorCallback ());
          m_nb.DelArpCache (l3->GetInterface (i)->GetArpCache ());
        }
    }
  
  // Close socket 
  Ptr<Socket> socket = FindSocketWithInterfaceAddress (m_ipv4->GetAddress (i, 0));
  NS_ASSERT (socket);
  socket->Close ();
  m_socketAddresses.erase (socket);
  if (m_socketAddresses.empty ())
    {
      NS_LOG_LOGIC ("No aodv interfaces");
      htimer.Cancel ();
      m_nb.Clear ();
      m_routingTable.Clear ();
      return;
    }
  m_routingTable.DeleteAllRoutesFromInterface (m_ipv4->GetAddress (i, 0));
}

void
RoutingProtocol::NotifyAddAddress (uint32_t i, Ipv4InterfaceAddress address)
{
  NS_LOG_FUNCTION (this << " interface " << i << " address " << address);
  Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
  if (!l3->IsUp (i))
    return;
  if (l3->GetNAddresses (i) == 1)
    {
      Ipv4InterfaceAddress iface = l3->GetAddress (i, 0);
      Ptr<Socket> socket = FindSocketWithInterfaceAddress (iface);
      if (!socket)
        {
          if (iface.GetLocal () == Ipv4Address ("127.0.0.1"))
            return;
          // Create a socket to listen only on this interface
          Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
              UdpSocketFactory::GetTypeId ());
          NS_ASSERT (socket != 0);
          socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvAodv,this));
          socket->Bind (InetSocketAddress (iface.GetLocal (), AODV_PORT));
          socket->Connect (InetSocketAddress (iface.GetBroadcast (), AODV_PORT));
          m_socketAddresses.insert (std::make_pair (socket, iface));

          // Add local broadcast record to the routing table
          Ptr<NetDevice> dev = m_ipv4->GetNetDevice (
              m_ipv4->GetInterfaceForAddress (iface.GetLocal ()));
          RoutingTableEntry rt (/*device=*/dev, /*dst=*/iface.GetBroadcast (), /*know seqno=*/true,
                                /*seqno=*/0, /*iface=*/iface, /*hops=*/1,
                                /*next hop=*/iface.GetBroadcast (), /*lifetime=*/Seconds (1e9)); // TODO use infty
          m_routingTable.AddRoute (rt);
        }
    }
  else
    {
      NS_LOG_LOGIC ("AODV does not work with more then one address per each interface. Ignore added address");
    }
}

void
RoutingProtocol::NotifyRemoveAddress (uint32_t i, Ipv4InterfaceAddress address)
{
  NS_LOG_FUNCTION (this);
  Ptr<Socket> socket = FindSocketWithInterfaceAddress (address);
  if (socket)
    {
      m_routingTable.DeleteAllRoutesFromInterface (address);
      m_socketAddresses.erase (socket);
      Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
      if (l3->GetNAddresses (i))
        {
          Ipv4InterfaceAddress iface = l3->GetAddress (i, 0);
          // Create a socket to listen only on this interface
          Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
              UdpSocketFactory::GetTypeId ());
          NS_ASSERT (socket != 0);
          socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvAodv, this));
          socket->Bind (InetSocketAddress (iface.GetLocal (), AODV_PORT));
          socket->Connect (InetSocketAddress (iface.GetBroadcast (), AODV_PORT));
          m_socketAddresses.insert (std::make_pair (socket, iface));

          // Add local broadcast record to the routing table
          Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (iface.GetLocal ()));
          RoutingTableEntry rt (/*device=*/dev, /*dst=*/iface.GetBroadcast (), /*know seqno=*/true, /*seqno=*/0, /*iface=*/iface,
                                /*hops=*/1, /*next hop=*/iface.GetBroadcast (), /*lifetime=*/Seconds (1e9)); // TODO use infty
          m_routingTable.AddRoute (rt);
        }
      if (m_socketAddresses.empty ())
        {
          NS_LOG_LOGIC ("No aodv interfaces");
          htimer.Cancel ();
          m_nb.Clear ();
          m_routingTable.Clear ();
          return;
        }
    }
  else
    {
      NS_LOG_LOGIC ("Remove address not participating in AODV operation");
    }
}

bool
RoutingProtocol::IsMyOwnAddress (Ipv4Address src)
{
  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator j =
      m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ipv4InterfaceAddress iface = j->second;
      if (src == iface.GetLocal ())
        {
          return true;
        }
    }
  return false;
}

void
RoutingProtocol::SendRequest (Ipv4Address dst)
{
  NS_LOG_FUNCTION ( this << dst);
  // A node SHOULD NOT originate more than RREQ_RATELIMIT RREQ messages per second.
  if (m_rreqCount == RreqRateLimit)
    {
      Simulator::Schedule (m_rreqRateLimitTimer.GetDelayLeft () + MilliSeconds (0.1),
          &RoutingProtocol::SendRequest, this, dst);
      return;
    }
  else
    m_rreqCount++;
  // Create RREQ header
  RreqHeader rreqHeader;
  rreqHeader.SetDst (dst);

  RoutingTableEntry rt;
  if (m_routingTable.LookupRoute (dst, rt))
    {
      rreqHeader.SetHopCount (rt.GetHop ());
      if (rt.GetValidSeqNo ())
        rreqHeader.SetDstSeqno (rt.GetSeqNo ());
      else
        rreqHeader.SetUnknownSeqno (true);
      rt.SetFlag (IN_SEARCH);
      m_routingTable.AddRoute (rt);
    }
  else
    {
      rreqHeader.SetUnknownSeqno (true);
      RoutingTableEntry newEntry;
      newEntry.SetFlag (IN_SEARCH);
      m_routingTable.AddRoute (newEntry);
    }

  if (GratuitousReply)
    rreqHeader.SetGratiousRrep (true);
  if (DestinationOnly)
    rreqHeader.SetDestinationOnly (true);

  m_seqNo++;
  rreqHeader.SetOriginSeqno (m_seqNo);
  m_requestId++;
  rreqHeader.SetId (m_requestId);
  rreqHeader.SetHopCount (0);

  // Send RREQ as subnet directed broadcast from each interface used by aodv
  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator j =
      m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ptr<Socket> socket = j->first;
      Ipv4InterfaceAddress iface = j->second;

      rreqHeader.SetOrigin (iface.GetLocal ());
      m_idCache.InsertId (iface.GetLocal (), m_requestId, PathDiscoveryTime);

      Ptr<Packet> packet = Create<Packet> ();
      packet->AddHeader (rreqHeader);
      TypeHeader tHeader (AODVTYPE_RREQ);
      packet->AddHeader (tHeader);
      socket->Send (packet);
    }
  ScheduleRreqRetry (dst);
  if (EnableHello)
    {
      htimer.Cancel ();
      htimer.Schedule (HelloInterval - Scalar (0.01) * MilliSeconds (UniformVariable ().GetValue (0, 10)));
    }
}

void
RoutingProtocol::ScheduleRreqRetry (Ipv4Address dst)
{
  if (m_addressReqTimer.find (dst) == m_addressReqTimer.end ())
    {
      Timer timer (Timer::CANCEL_ON_DESTROY);
      m_addressReqTimer[dst] = timer;
    }
  m_addressReqTimer[dst].SetFunction (&RoutingProtocol::RouteRequestTimerExpire, this);
  m_addressReqTimer[dst].Remove ();
  m_addressReqTimer[dst].SetArguments (dst);
  RoutingTableEntry rt;
  m_routingTable.LookupRoute (dst, rt);
  rt.IncrementRreqCnt ();
  m_routingTable.Update (rt);
  m_addressReqTimer[dst].Schedule (Scalar (rt.GetRreqCnt ()) * NetTraversalTime);
}

void
RoutingProtocol::RecvAodv (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this);
  Address sourceAddress;
  Ptr<Packet> packet = socket->RecvFrom (sourceAddress);
  InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom (sourceAddress);
  Ipv4Address sender = inetSourceAddr.GetIpv4 ();
  Ipv4Address receiver = m_socketAddresses[socket].GetLocal ();
  NS_LOG_DEBUG ("AODV node " << this << " received a AODV packet from " << sender << " to " << receiver);

  UpdateRouteToNeighbor (sender, receiver);
  TypeHeader tHeader (AODVTYPE_RREQ);
  packet->RemoveHeader (tHeader);
  if (!tHeader.IsValid ())
    {
      NS_LOG_DEBUG ("AODV message " << packet->GetUid() << " with unknown type received: " << tHeader.Get() << ". Drop");
      return; // drop
    }
  switch (tHeader.Get ())
    {
    case AODVTYPE_RREQ:
      {
        RecvRequest (packet, receiver, sender);
        break;
      }
    case AODVTYPE_RREP:
      {
        RecvReply (packet, receiver, sender);
        break;
      }
    case AODVTYPE_RERR:
      {
        RecvError (packet, sender);
        break;
      }
    case AODVTYPE_RREP_ACK:
      {
        RecvReplyAck (sender);
        break;
      }
    }
}

bool
RoutingProtocol::UpdateRouteLifeTime (Ipv4Address addr, Time lifetime)
{
  RoutingTableEntry rt;
  if (m_routingTable.LookupRoute (addr, rt))
    {
      rt.SetFlag (VALID);
      rt.SetRreqCnt (0);
      rt.SetLifeTime (std::max (lifetime, rt.GetLifeTime ()));
      m_routingTable.Update (rt);
      return true;
    }
  return false;
}

void
RoutingProtocol::UpdateRouteToNeighbor (Ipv4Address sender, Ipv4Address receiver)
{
  NS_LOG_FUNCTION (this << "sender " << sender << " receiver " << receiver);
  RoutingTableEntry toNeighbor;
  if (!m_routingTable.LookupRoute (sender, toNeighbor))
    {
      Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (receiver));
      RoutingTableEntry newEntry (/*device=*/dev, /*dst=*/sender, /*know seqno=*/false, /*seqno=*/0,
                                  /*iface=*/m_ipv4->GetAddress (m_ipv4->GetInterfaceForAddress (receiver), 0),
                                  /*hops=*/1, /*next hop=*/sender, /*lifetime=*/ActiveRouteTimeout);
      m_routingTable.AddRoute (newEntry);
    }
  else
    {
      Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (receiver));
      RoutingTableEntry newEntry (/*device=*/dev, /*dst=*/sender, /*know seqno=*/false, /*seqno=*/0,
                                  /*iface=*/m_ipv4->GetAddress (m_ipv4->GetInterfaceForAddress (receiver), 0),
                                  /*hops=*/1, /*next hop=*/sender, /*lifetime=*/std::max (ActiveRouteTimeout, toNeighbor.GetLifeTime ()));
      m_routingTable.Update (newEntry);
    }
}

void
RoutingProtocol::RecvRequest (Ptr<Packet> p, Ipv4Address receiver, Ipv4Address src)
{
  NS_LOG_FUNCTION (this);
  RreqHeader rreqHeader;
  p->RemoveHeader (rreqHeader);

  // A node ignores all RREQs received from any node in its blacklist
  RoutingTableEntry toPrev;
  if (m_routingTable.LookupRoute (src, toPrev))
    {
      if (toPrev.IsUnidirectional ())
        return;
    }

  uint32_t id = rreqHeader.GetId ();
  Ipv4Address origin = rreqHeader.GetOrigin ();

  /*
   *  Node checks to determine whether it has received a RREQ with the same Originator IP Address and RREQ ID.
   *  If such a RREQ has been received, the node silently discards the newly received RREQ.
   */
  if (m_idCache.LookupId (origin, id))
    {
      return;
    }
  m_idCache.InsertId (origin, id, PathDiscoveryTime);

  // Increment RREQ hop count
  uint8_t hop = rreqHeader.GetHopCount () + 1;
  rreqHeader.SetHopCount (hop);

  /*
   *  When the reverse route is created or updated, the following actions on the route are also carried out:
   *  1. the Originator Sequence Number from the RREQ is compared to the corresponding destination sequence number
   *     in the route table entry and copied if greater than the existing value there
   *  2. the valid sequence number field is set to true;
   *  3. the next hop in the routing table becomes the node from which the  RREQ was received
   *  4. the hop count is copied from the Hop Count in the RREQ message;
   *  5. the Lifetime is set to be the maximum of (ExistingLifetime, MinimalLifetime), where
   *     MinimalLifetime = current time + 2*NetTraversalTime - 2*HopCount*NodeTraversalTime
   */
  RoutingTableEntry toOrigin;
  if (!m_routingTable.LookupRoute (origin, toOrigin))
    {
      Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (receiver));
      RoutingTableEntry newEntry (/*device=*/dev, /*dst=*/origin, /*validSeno=*/true, /*seqNo=*/rreqHeader.GetOriginSeqno (),
                                  /*iface=*/m_ipv4->GetAddress (m_ipv4->GetInterfaceForAddress (receiver), 0), /*hops=*/hop,
                                  /*nextHop*/src, /*timeLife=*/Scalar (2) * NetTraversalTime - Scalar (2 * hop) * NodeTraversalTime);
      m_routingTable.AddRoute (newEntry);
    }
  else
    {
      if (toOrigin.GetValidSeqNo ())
        {
          if (int32_t (rreqHeader.GetOriginSeqno ()) - int32_t (toOrigin.GetSeqNo ()) > 0)
            toOrigin.SetSeqNo (rreqHeader.GetOriginSeqno ());
        }
      else
        toOrigin.SetSeqNo (rreqHeader.GetOriginSeqno ());
      toOrigin.SetValidSeqNo (true);
      toOrigin.SetNextHop (src);
      toOrigin.SetOutputDevice (m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (receiver)));
      toOrigin.SetInterface (m_ipv4->GetAddress (m_ipv4->GetInterfaceForAddress (receiver), 0));
      toOrigin.SetHop (hop);
      toOrigin.SetLifeTime (std::max (Scalar (2) * NetTraversalTime - Scalar (2 * hop) * NodeTraversalTime, toOrigin.GetLifeTime ()));
      m_routingTable.Update (toOrigin);
    }
  NS_LOG_LOGIC (receiver << " receive RREQ to destination " << rreqHeader.GetDst ());

  //  A node generates a RREP if either:
  //  (i)  it is itself the destination,
  if (IsMyOwnAddress (rreqHeader.GetDst ()))
    {
      m_routingTable.LookupRoute (origin, toOrigin);
      SendReply (rreqHeader, toOrigin);
      return;
    }
  /*
   * (ii) or it has an active route to the destination, the destination sequence number in the node's existing route table entry for the destination
   *      is valid and greater than or equal to the Destination Sequence Number of the RREQ, and the "destination only" flag is NOT set.
   */
  RoutingTableEntry toDst;
  Ipv4Address dst = rreqHeader.GetDst ();
  if (m_routingTable.LookupRoute (dst, toDst))
    {
      /*
       * The Destination Sequence number for the requested destination is set to the maximum of the corresponding value
       * received in the RREQ message, and the destination sequence value currently maintained by the node for the requested destination.
       * However, the forwarding node MUST NOT modify its maintained value for the destination sequence number, even if the value
       * received in the incoming RREQ is larger than the value currently maintained by the forwarding node.
       */
      if (rreqHeader.GetUnknownSeqno () || ( (int32_t (toDst.GetSeqNo ()) - int32_t (rreqHeader.GetDstSeqno ()) > 0)
          && toDst.GetValidSeqNo () ))
        {
          if (!rreqHeader.GetDestinationOnly () && toDst.GetFlag() == VALID)
            {
              m_routingTable.LookupRoute (origin, toOrigin);
              SendReplyByIntermediateNode (toDst, toOrigin, rreqHeader.GetGratiousRrep ());
              return;
            }
          rreqHeader.SetDstSeqno (toDst.GetSeqNo ());
          rreqHeader.SetUnknownSeqno (false);
        }
    }

  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator j =
      m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ptr<Socket> socket = j->first;
      Ipv4InterfaceAddress iface = j->second;
      Ptr<Packet> packet = Create<Packet> ();
      packet->AddHeader (rreqHeader);
      TypeHeader tHeader (AODVTYPE_RREQ);
      packet->AddHeader (tHeader);
      socket->Send (packet);
    }

  if (EnableHello)
    {
      htimer.Cancel ();
      htimer.Schedule (HelloInterval - Scalar(0.1)*MilliSeconds(UniformVariable().GetValue (0.0, 10.0)));
    }
}

void
RoutingProtocol::SendReply (RreqHeader const & rreqHeader, RoutingTableEntry const & toOrigin)
{
  NS_LOG_FUNCTION (this << toOrigin.GetDestination ());
  /*
   * Destination node MUST increment its own sequence number by one if the sequence number in the RREQ packet is equal to that
   * incremented value. Otherwise, the destination does not change its sequence number before generating the  RREP message.
   */
  if (!rreqHeader.GetUnknownSeqno () && (rreqHeader.GetDstSeqno () == m_seqNo + 1))
    m_seqNo++;
  RrepHeader rrepHeader ( /*prefixSize=*/0, /*hops=*/toOrigin.GetHop (), /*dst=*/rreqHeader.GetDst (),
                          /*dstSeqNo=*/m_seqNo, /*origin=*/toOrigin.GetDestination (), /*lifeTime=*/MyRouteTimeout);
  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (rrepHeader);
  TypeHeader tHeader (AODVTYPE_RREP);
  packet->AddHeader (tHeader);
  Ptr<Socket> socket = FindSocketWithInterfaceAddress (toOrigin.GetInterface ());
  NS_ASSERT (socket);
  socket->SendTo (packet, 0, InetSocketAddress (toOrigin.GetNextHop (), AODV_PORT));
}

void
RoutingProtocol::SendReplyByIntermediateNode (RoutingTableEntry & toDst, RoutingTableEntry & toOrigin, bool gratRep)
{
  NS_LOG_FUNCTION(this);
  RrepHeader rrepHeader (/*prefix size=*/0, /*hops=*/toDst.GetHop (), /*dst=*/toDst.GetDestination (), /*dst seqno=*/toDst.GetSeqNo (),
                         /*origin=*/toOrigin.GetDestination (), /*lifetime=*/toDst.GetLifeTime ());
  /* If the node we received a RREQ for is a neighbor we are
   * probably facing a unidirectional link... Better request a RREP-ack
   */
  if (toDst.GetHop () == 1)
    {
      rrepHeader.SetAckRequired (true);
      RoutingTableEntry toNextHop;
      m_routingTable.LookupRoute (toOrigin.GetNextHop (), toNextHop);
      toNextHop.m_ackTimer.SetFunction (&RoutingProtocol::AckTimerExpire, this);
      toNextHop.m_ackTimer.SetArguments (toNextHop.GetDestination (), BlackListTimeout);
      toNextHop.m_ackTimer.SetDelay (NextHopWait);
    }
  toDst.InsertPrecursor (toOrigin.GetNextHop ());
  toOrigin.InsertPrecursor (toDst.GetNextHop ());
  m_routingTable.Update (toDst);
  m_routingTable.Update (toOrigin);

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (rrepHeader);
  TypeHeader tHeader (AODVTYPE_RREP);
  packet->AddHeader (tHeader);
  Ptr<Socket> socket = FindSocketWithInterfaceAddress (toOrigin.GetInterface ());
  NS_ASSERT (socket);
  socket->SendTo (packet, 0, InetSocketAddress (toOrigin.GetNextHop (), AODV_PORT));

  // Generating gratuitous RREPs
  if (gratRep)
    {
      RrepHeader gratRepHeader (/*prefix size=*/0, /*hops=*/toOrigin.GetHop (), /*dst=*/toOrigin.GetDestination (),
                                /*dst seqno=*/toOrigin.GetSeqNo (), /*origin=*/toDst.GetDestination (),
                                /*lifetime=*/toOrigin.GetLifeTime ());
      Ptr<Packet> packetToDst = Create<Packet> ();
      packetToDst->AddHeader (gratRepHeader);
      TypeHeader type (AODVTYPE_RREP);
      packetToDst->AddHeader (type);
      Ptr<Socket> socket = FindSocketWithInterfaceAddress (toDst.GetInterface ());
      NS_ASSERT (socket);
      NS_LOG_LOGIC ("Send gratuitous RREP " << packet->GetUid());
      socket->SendTo (packetToDst, 0, InetSocketAddress (toDst.GetNextHop (), AODV_PORT));
    }
}

void
RoutingProtocol::SendReplyAck (Ipv4Address neighbor)
{
  NS_LOG_FUNCTION (this << " to " << neighbor);
  RrepAckHeader h;
  TypeHeader typeHeader (AODVTYPE_RREP_ACK);
  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (h);
  packet->AddHeader (typeHeader);
  RoutingTableEntry toNeighbor;
  m_routingTable.LookupRoute (neighbor, toNeighbor);
  Ptr<Socket> socket = FindSocketWithInterfaceAddress (toNeighbor.GetInterface ());
  NS_ASSERT (socket);
  socket->SendTo (packet, 0, InetSocketAddress (neighbor, AODV_PORT));
}

void
RoutingProtocol::RecvReply (Ptr<Packet> p, Ipv4Address receiver, Ipv4Address sender)
{
  NS_LOG_FUNCTION(this << " src " << sender);
  RrepHeader rrepHeader;
  p->RemoveHeader (rrepHeader);
  Ipv4Address dst = rrepHeader.GetDst ();
  NS_LOG_LOGIC("RREP destination " << dst << " RREP origin " << rrepHeader.GetOrigin());

  uint8_t hop = rrepHeader.GetHopCount () + 1;
  rrepHeader.SetHopCount (hop);

  // If RREP is Hello message
  if (dst == rrepHeader.GetOrigin ())
    {
      ProcessHello (rrepHeader, receiver);
      return;
    }

  /*
   * If the route table entry to the destination is created or updated, then the following actions occur:
   * -  the route is marked as active,
   * -  the destination sequence number is marked as valid,
   * -  the next hop in the route entry is assigned to be the node from which the RREP is received,
   *    which is indicated by the source IP address field in the IP header,
   * -  the hop count is set to the value of the hop count from RREP message + 1
   * -  the expiry time is set to the current time plus the value of the Lifetime in the RREP message,
   * -  and the destination sequence number is the Destination Sequence Number in the RREP message.
   */
  Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (receiver));
  RoutingTableEntry newEntry (/*device=*/dev, /*dst=*/dst, /*validSeqNo=*/true, /*seqno=*/rrepHeader.GetDstSeqno (),
                              /*iface=*/m_ipv4->GetAddress (m_ipv4->GetInterfaceForAddress (receiver), 0),/*hop=*/hop,
                              /*nextHop=*/sender, /*lifeTime=*/rrepHeader.GetLifeTime ());
  RoutingTableEntry toDst;
  if (m_routingTable.LookupRoute (dst, toDst))
    {
      /*
       * The existing entry is updated only in the following circumstances:
       * (i) the sequence number in the routing table is marked as invalid in route table entry.
       */
      if (!toDst.GetValidSeqNo ())
        {
          m_routingTable.Update (newEntry);
        }
      // (ii)the Destination Sequence Number in the RREP is greater than the node's copy of the destination sequence number and the known value is valid,
      else if ((int32_t (rrepHeader.GetDstSeqno ()) - int32_t (toDst.GetSeqNo ())) > 0)
        {
          m_routingTable.Update (newEntry);
        }
      else
        {
          // (iii) the sequence numbers are the same, but the route is marked as inactive.
          if ((rrepHeader.GetDstSeqno () == toDst.GetSeqNo ()) && (toDst.GetFlag () != VALID))
            {
              m_routingTable.Update (newEntry);
            }
          // (iv)  the sequence numbers are the same, and the New Hop Count is smaller than the hop count in route table entry.
          else if ((rrepHeader.GetDstSeqno () == toDst.GetSeqNo ()) && (hop < toDst.GetHop ()))
            {
              m_routingTable.Update (newEntry);
            }
        }
    }
  else
    {
      // The forward route for this destination is created if it does not already exist.
      NS_LOG_LOGIC("add new route");
      m_routingTable.AddRoute (newEntry);
    }
  // Acknowledge receipt of the RREP by sending a RREP-ACK message back
  if (rrepHeader.GetAckRequired ())
    {
      SendReplyAck (sender);
      rrepHeader.SetAckRequired (false);
    }
  NS_LOG_LOGIC ("receiver " << receiver << " origin " << rrepHeader.GetOrigin ());
  if (IsMyOwnAddress (rrepHeader.GetOrigin ()))
    {
      if (toDst.GetFlag () == IN_SEARCH)
        {
          m_routingTable.Update (newEntry);
          m_addressReqTimer[dst].Remove ();
          m_addressReqTimer.erase (dst);
        }
      SendPacketFromQueue (rrepHeader.GetDst (), newEntry.GetRoute ());
      return;
    }

  RoutingTableEntry toOrigin;
  if (!m_routingTable.LookupRoute (rrepHeader.GetOrigin (), toOrigin))
    {
      return; // Impossible! drop.
    }
  toOrigin.SetLifeTime (std::max (ActiveRouteTimeout, toOrigin.GetLifeTime ()));
  m_routingTable.Update (toOrigin);

  // Update information about precursors
  m_routingTable.LookupRoute (rrepHeader.GetDst (), toDst);
  toDst.InsertPrecursor (toOrigin.GetNextHop ());
  m_routingTable.Update (toDst);

  RoutingTableEntry toNextHopToDst;
  m_routingTable.LookupRoute (toDst.GetNextHop (), toNextHopToDst);
  toNextHopToDst.InsertPrecursor (toOrigin.GetNextHop ());
  m_routingTable.Update (toNextHopToDst);

  toOrigin.InsertPrecursor (toDst.GetNextHop ());
  m_routingTable.Update (toOrigin);

  RoutingTableEntry toNextHopToOrigin;
  m_routingTable.LookupRoute (toOrigin.GetNextHop (), toNextHopToOrigin);
  toNextHopToOrigin.InsertPrecursor (toDst.GetNextHop ());
  m_routingTable.Update (toNextHopToOrigin);

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (rrepHeader);
  TypeHeader tHeader (AODVTYPE_RREP);
  packet->AddHeader (tHeader);
  Ptr<Socket> socket = FindSocketWithInterfaceAddress (toOrigin.GetInterface ());
  NS_ASSERT (socket);
  socket->SendTo (packet, 0, InetSocketAddress (toOrigin.GetNextHop (), AODV_PORT));
}

void
RoutingProtocol::RecvReplyAck (Ipv4Address neighbor)
{
  NS_LOG_FUNCTION (this);
  RoutingTableEntry rt;
  if(m_routingTable.LookupRoute(neighbor, rt))
    {
      rt.m_ackTimer.Cancel ();
      rt.SetFlag (VALID);
      m_routingTable.Update(rt);
    }
}

void
RoutingProtocol::ProcessHello (RrepHeader const & rrepHeader, Ipv4Address receiver )
{
  NS_LOG_FUNCTION(this << "from " << rrepHeader.GetDst ());
  /*
   *  Whenever a node receives a Hello message from a neighbor, the node
   * SHOULD make sure that it has an active route to the neighbor, and
   * create one if necessary.
   */
  RoutingTableEntry toNeighbor;
  if (!m_routingTable.LookupRoute (rrepHeader.GetDst (), toNeighbor))
    {
      Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (receiver));
      RoutingTableEntry newEntry (/*device=*/dev, /*dst=*/rrepHeader.GetDst (), /*validSeqNo=*/true, /*seqno=*/rrepHeader.GetDstSeqno (),
                                  /*iface=*/m_ipv4->GetAddress (m_ipv4->GetInterfaceForAddress (receiver), 0),
                                  /*hop=*/1, /*nextHop=*/rrepHeader.GetDst (), /*lifeTime=*/rrepHeader.GetLifeTime ());
      m_routingTable.AddRoute (newEntry);
    }
  else
    {
      toNeighbor.SetLifeTime (std::max (Scalar (AllowedHelloLoss) * HelloInterval, toNeighbor.GetLifeTime ()));
      toNeighbor.SetSeqNo (rrepHeader.GetDstSeqno ());
      toNeighbor.SetValidSeqNo (true);
      toNeighbor.SetFlag (VALID);
      toNeighbor.SetOutputDevice (m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (receiver)));
      toNeighbor.SetInterface (m_ipv4->GetAddress (m_ipv4->GetInterfaceForAddress (receiver), 0));
      m_routingTable.Update (toNeighbor);
    }
  if (EnableHello)
    {
      m_nb.Update (rrepHeader.GetDst (), Scalar (AllowedHelloLoss) * HelloInterval);
    }
}

void
RoutingProtocol::RecvError (Ptr<Packet> p, Ipv4Address src )
{
  NS_LOG_FUNCTION (this << " from " << src);
  RerrHeader rerrHeader;
  p->RemoveHeader (rerrHeader);
  std::map<Ipv4Address, uint32_t> dstWithNextHopSrc;
  std::map<Ipv4Address, uint32_t> unreachable;
  m_routingTable.GetListOfDestinationWithNextHop (src, dstWithNextHopSrc);
  std::pair<Ipv4Address, uint32_t> un;
  while (rerrHeader.RemoveUnDestination (un))
    {
      if (m_nb.IsNeighbor (un.first))
        SendRerrWhenBreaksLinkToNextHop (un.first);
      else
        {
          for (std::map<Ipv4Address, uint32_t>::const_iterator i =
              dstWithNextHopSrc.begin (); i != dstWithNextHopSrc.end (); ++i)
            if (i->first == un.first)
              {
                Ipv4Address dst = un.first;
                unreachable.insert (un);
              }
        }
    }

  std::vector<Ipv4Address> precursors;
  for (std::map<Ipv4Address, uint32_t>::const_iterator i = unreachable.begin ();
      i != unreachable.end ();)
    {
      if (!rerrHeader.AddUnDestination (i->first, i->second))
        {
          TypeHeader typeHeader (AODVTYPE_RERR);
          Ptr<Packet> packet = Create<Packet> ();
          packet->AddHeader (rerrHeader);
          packet->AddHeader (typeHeader);
          SendRerrMessage (packet, precursors);
          rerrHeader.Clear ();
        }
      else
        {
          RoutingTableEntry toDst;
          m_routingTable.LookupRoute (i->first, toDst);
          toDst.GetPrecursors (precursors);
          ++i;
        }
    }
  if (rerrHeader.GetDestCount () != 0)
    {
      TypeHeader typeHeader (AODVTYPE_RERR);
      Ptr<Packet> packet = Create<Packet> ();
      packet->AddHeader (rerrHeader);
      packet->AddHeader (typeHeader);
      SendRerrMessage (packet, precursors);
    }
  m_routingTable.InvalidateRoutesWithDst (unreachable);
}

void
RoutingProtocol::RouteRequestTimerExpire (Ipv4Address dst)
{
  NS_LOG_LOGIC(this);
  RoutingTableEntry toDst;
  m_routingTable.LookupRoute (dst, toDst);
  if (toDst.GetFlag () == VALID)
    {
      SendPacketFromQueue (dst, toDst.GetRoute ());
      NS_LOG_LOGIC ("route to " << dst << " found");
      return;
    }
  /*
   *  If a route discovery has been attempted RreqRetries times at the maximum TTL without
   *  receiving any RREP, all data packets destined for the corresponding destination SHOULD be
   *  dropped from the buffer and a Destination Unreachable message SHOULD be delivered to the application.
   */
  if (toDst.GetRreqCnt () == RreqRetries)
    {
      NS_LOG_LOGIC("route discovery to " << dst << " has been attempted RreqRetries times");
      m_addressReqTimer.erase (dst);
      m_routingTable.DeleteRoute (dst);
      NS_LOG_DEBUG ("Route not found. Drop packet with dst " << dst);
      m_queue.DropPacketWithDst (dst);
      return;
    }

  if (toDst.GetFlag () == IN_SEARCH)
    {
      NS_LOG_LOGIC ("Send new RREQ to " << dst << " ttl " << NetDiameter);
      SendRequest (dst);
    }
  else
    {
      NS_LOG_DEBUG ("Route down. Stop search. Drop packet with destination " << dst);
      m_addressReqTimer.erase(dst);
      m_routingTable.DeleteRoute(dst);
      m_queue.DropPacketWithDst(dst);
    }
}

void
RoutingProtocol::HelloTimerExpire ()
{
  NS_LOG_FUNCTION(this);
  SendHello ();
  htimer.Cancel ();
  Time t = Scalar(0.01)*MilliSeconds(UniformVariable().GetValue (0.0, 100.0));
  htimer.Schedule (HelloInterval - t);
}

void
RoutingProtocol::RreqRateLimitTimerExpire ()
{
  m_rreqCount = 0;
  m_rreqRateLimitTimer.Schedule (Seconds (1));
}

void
RoutingProtocol::AckTimerExpire (Ipv4Address neighbor, Time blacklistTimeout)
{
  NS_LOG_FUNCTION(this);
  m_routingTable.MarkLinkAsUinidirectional (neighbor, blacklistTimeout);
}

void
RoutingProtocol::SendHello ()
{
  NS_LOG_FUNCTION(this);
  /* Broadcast a RREP with TTL = 1 with the RREP message fields set as follows:
   *   Destination IP Address         The node's IP address.
   *   Destination Sequence Number    The node's latest sequence number.
   *   Hop Count                      0
   *   Lifetime                       AllowedHelloLoss * HelloInterval
   */
  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator j = m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ptr<Socket> socket = j->first;
      Ipv4InterfaceAddress iface = j->second;
      RrepHeader helloHeader (/*prefix size=*/0, /*hops=*/0, /*dst=*/iface.GetLocal (), /*dst seqno=*/m_seqNo,
                              /*origin=*/iface.GetLocal (),/*lifetime=*/Scalar (AllowedHelloLoss) * HelloInterval);
      Ptr<Packet> packet = Create<Packet> ();
      packet->AddHeader (helloHeader);
      TypeHeader tHeader (AODVTYPE_RREP);
      packet->AddHeader (tHeader);
      socket->Send (packet);
    }
}

void
RoutingProtocol::SendPacketFromQueue (Ipv4Address dst, Ptr<Ipv4Route> route)
{
  NS_LOG_FUNCTION(this);
  QueueEntry queueEntry;
  while (m_queue.Dequeue (dst, queueEntry))
    {
      UnicastForwardCallback ucb = queueEntry.GetUnicastForwardCallback ();
      ucb (route, queueEntry.GetPacket (), queueEntry.GetIpv4Header ());
    }
}

void
RoutingProtocol::Send (Ptr<Ipv4Route> route, Ptr<const Packet> packet,
    const Ipv4Header & header)
{
  NS_LOG_FUNCTION (this << packet->GetUid() << (uint16_t) header.GetProtocol());
  Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
  NS_ASSERT(l3 != 0);
  Ptr<Packet> p = packet->Copy ();
  l3->Send (p, route->GetSource (), header.GetDestination (),
      header.GetProtocol (), route);
}

void
RoutingProtocol::SendRerrWhenBreaksLinkToNextHop (Ipv4Address nextHop)
{
  NS_LOG_FUNCTION (this << nextHop);
  RerrHeader rerrHeader;
  std::vector<Ipv4Address> precursors;
  std::map<Ipv4Address, uint32_t> unreachable;

  RoutingTableEntry toNextHop;
  if (!m_routingTable.LookupRoute (nextHop, toNextHop))
    return;
  toNextHop.GetPrecursors (precursors);
  rerrHeader.AddUnDestination (nextHop, toNextHop.GetSeqNo ());
  m_routingTable.GetListOfDestinationWithNextHop (nextHop, unreachable);
  for (std::map<Ipv4Address, uint32_t>::const_iterator i = unreachable.begin (); i
      != unreachable.end ();)
    {
      if (!rerrHeader.AddUnDestination (i->first, i->second))
        {
          NS_LOG_LOGIC ("Send RERR message with maximum size.");
          TypeHeader typeHeader (AODVTYPE_RERR);
          Ptr<Packet> packet = Create<Packet> ();
          packet->AddHeader (rerrHeader);
          packet->AddHeader (typeHeader);
          SendRerrMessage (packet, precursors);
          rerrHeader.Clear ();
        }
      else
        {
          RoutingTableEntry toDst;
          m_routingTable.LookupRoute (i->first, toDst);
          toDst.GetPrecursors (precursors);
          ++i;
        }
    }
  if (rerrHeader.GetDestCount () != 0)
    {
      TypeHeader typeHeader (AODVTYPE_RERR);
      Ptr<Packet> packet = Create<Packet> ();
      packet->AddHeader (rerrHeader);
      packet->AddHeader (typeHeader);
      SendRerrMessage (packet, precursors);
    }
  unreachable.insert (std::make_pair (nextHop, toNextHop.GetSeqNo ()));
  m_routingTable.InvalidateRoutesWithDst (unreachable);
}

void
RoutingProtocol::SendRerrWhenNoRouteToForward (Ipv4Address dst,
    uint32_t dstSeqNo, Ipv4Address origin)
{
  NS_LOG_FUNCTION (this);
  RerrHeader rerrHeader;
  rerrHeader.AddUnDestination (dst, dstSeqNo);
  RoutingTableEntry toOrigin;
  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (rerrHeader);
  packet->AddHeader (TypeHeader (AODVTYPE_RERR));
  if (m_routingTable.LookupRoute (origin, toOrigin))
    {
      if (toOrigin.GetFlag () == VALID)
        {
          Ptr<Socket> socket = FindSocketWithInterfaceAddress (
              toOrigin.GetInterface ());
          NS_ASSERT (socket);
          NS_LOG_LOGIC ("Unicast RERR to the source of the data transmission");
          socket->SendTo (packet, 0, InetSocketAddress (toOrigin.GetNextHop (), AODV_PORT));
        }

    }
  else
    {
      for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator i =
          m_socketAddresses.begin (); i != m_socketAddresses.end (); ++i)
        {
          Ptr<Socket> socket = i->first;
          Ipv4InterfaceAddress iface = i->second;
          NS_ASSERT (socket);
          NS_LOG_LOGIC ("Broadcast RERR message from interface " << iface.GetLocal());
          socket->Send (packet);
        }
    }
}

void
RoutingProtocol::SendRerrMessage (Ptr<Packet> packet, std::vector<Ipv4Address> precursors)
{
  NS_LOG_FUNCTION(this);

  if (precursors.empty ())
    {
      NS_LOG_LOGIC ("No precursors");
      return;
    }
  // If there is only one precursor, RERR SHOULD be unicast toward that precursor
  if (precursors.size () == 1)
    {
      RoutingTableEntry toPrecursor;
      if (!m_routingTable.LookupRoute (precursors.front (), toPrecursor))
        return;
      Ptr<Socket> socket = FindSocketWithInterfaceAddress (toPrecursor.GetInterface ());
      NS_ASSERT (socket);
      if (toPrecursor.GetFlag () == VALID)
        {
          NS_LOG_LOGIC ("one precursor => unicast RERR to " << toPrecursor.GetDestination() << " from " << toPrecursor.GetInterface ().GetLocal ());
          socket->SendTo (packet, 0, InetSocketAddress (precursors.front (), AODV_PORT));
        }
      else
        NS_LOG_LOGIC ("One precursor, but no valid route to this precursor");
      return;
    }

  //  Should only transmit RERR on those interfaces which have precursor nodes for the broken route
  std::vector<Ipv4InterfaceAddress> ifaces;
  RoutingTableEntry toPrecursor;
  for (std::vector<Ipv4Address>::const_iterator i = precursors.begin (); i
      != precursors.end (); ++i)
    {
      if (!m_routingTable.LookupRoute (*i, toPrecursor))
        break;
      bool result = true;
      for (std::vector<Ipv4InterfaceAddress>::const_iterator i =
          ifaces.begin (); i != ifaces.end (); ++i)
        if (*i == toPrecursor.GetInterface ())
          {
            result = false;
            break;
          }
      if (result)
        ifaces.push_back (toPrecursor.GetInterface ());
    }

  for (std::vector<Ipv4InterfaceAddress>::const_iterator i = ifaces.begin (); i != ifaces.end (); ++i)
    {
      Ptr<Socket> socket = FindSocketWithInterfaceAddress (*i);
      NS_ASSERT (socket);
      NS_LOG_LOGIC ("Broadcast RERR message from interface " << i->GetLocal());
      socket->Send (packet);
    }

}

Ptr<Socket>
RoutingProtocol::FindSocketWithInterfaceAddress (Ipv4InterfaceAddress addr ) const
{
  for (std::map<Ptr<Socket> , Ipv4InterfaceAddress>::const_iterator j =
      m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
    {
      Ptr<Socket> socket = j->first;
      Ipv4InterfaceAddress iface = j->second;
      if (iface == addr)
        return socket;
    }
  Ptr<Socket> socket;
  return socket;
}

void
RoutingProtocol::Drop(Ptr<const Packet> packet, const Ipv4Header & header, Socket::SocketErrno err)
{
  NS_LOG_DEBUG (this <<" drop own packet " << packet->GetUid() << " to " << header.GetDestination () << " from queue. Error " << err);
}

}
}
