/*
 *  Copyright (C) 2007 Austin Robot Technology, Patrick Beeson
 *  Copyright (C) 2009, 2010 Austin Robot Technology, Jack O'Quin
 *
 *  License: Modified BSD Software License Agreement
 *
 *  $Id$
 */

/** \file
 *
 *  Input classes for the Velodyne HDL-64E 3D LIDAR:
 * 
 *     Input -- virtual base class than can be used to access the data
 *              independently of its source
 *
 *     InputSocket -- derived class reads live data from the device
 *              via a UDP socket
 *
 *     InputPCAP -- derived class provides a similar interface from a
 *              PCAP dump
 */

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>

#include <velodyne_driver/input.h>

namespace velodyne_driver
{
  static const size_t packet_size = sizeof(velodyne_msgs::VelodynePacket::data);

  ////////////////////////////////////////////////////////////////////////
  // Input class implementation
  ////////////////////////////////////////////////////////////////////////

  /** \brief Get one velodyne packet. */
  int Input::getPacket(velodyne_msgs::VelodynePacket *pkt)
  {
    double time = 0.0;
    int rc = getPackets(&pkt->data[0], 1, &time);
    pkt->stamp = ros::Time(time);
    return rc;
  }

  ////////////////////////////////////////////////////////////////////////
  // InputSocket class implementation
  ////////////////////////////////////////////////////////////////////////

  /** @brief constructor */
  InputSocket::InputSocket(ros::NodeHandle private_nh, uint16_t udp_port):
    Input(private_nh)
  {
    udp_port_ = udp_port;
    sockfd_ = -1;
  }

  /** @ brief Connect to Velodyne UDP port
   *
   * returns: socket file descriptor number >= 0, if successful
   *          -1, for failure
   */
  int InputSocket::vopen(void)
  {
    ROS_INFO_STREAM("Opening UDP socket: port " << udp_port_);

    sockfd_ = socket(PF_INET, SOCK_DGRAM, 0);
    if (sockfd_ == -1)
      {
        perror("socket");               // TODO: ROS_ERROR errno
        return -1;
      }
  
    sockaddr_in my_addr;                     // my address information
    memset(&my_addr, 0, sizeof(my_addr));    // initialize to zeros
    my_addr.sin_family = AF_INET;            // host byte order
    my_addr.sin_port = htons(udp_port_);     // short, in network byte order
    my_addr.sin_addr.s_addr = INADDR_ANY;    // automatically fill in my IP
  
    if (bind(sockfd_, (sockaddr *)&my_addr, sizeof(sockaddr)) == -1)
      {
        perror("bind");                 // TODO: ROS_ERROR errno
        return -1;
      }
  
    ROS_DEBUG("Velodyne socket fd is %d\n", sockfd_);

    return 0;
  }

  /** Read velodyne packets from socket. */
  int InputSocket::getPackets(uint8_t *buffer, int npacks,
                                      double *data_time)
  {
    int result = npacks;
    double time1 = ros::Time::now().toSec();

    struct pollfd fds[1];
    fds[0].fd = sockfd_;
    fds[0].events = POLLIN;
    static const int POLL_TIMEOUT = 1000; // one second (in msec)

    for (int i = 0; i < npacks; ++i)
      {
        // Unfortunately, the Linux kernel recvfrom() implementation
        // uses a non-interruptible sleep() when waiting for data,
        // which would cause this method to hang if the device is not
        // providing data.  We poll() the device first to make sure
        // the recvfrom() will not block.
        //
        // Note, however, that there is a known Linux kernel bug:
        //
        //   Under Linux, select() may report a socket file descriptor
        //   as "ready for reading", while nevertheless a subsequent
        //   read blocks.  This could for example happen when data has
        //   arrived but upon examination has wrong checksum and is
        //   discarded.  There may be other circumstances in which a
        //   file descriptor is spuriously reported as ready.  Thus it
        //   may be safer to use O_NONBLOCK on sockets that should not
        //   block.

        // poll() until input available
        do
          {
            int retval = poll(fds, 1, POLL_TIMEOUT);
            if (retval < 0)             // poll() error?
              {
                if (errno != EINTR)
                  ROS_ERROR("poll() error: %s", strerror(errno));
                return result;
              }
            if (retval == 0)            // poll() timeout?
              {
                ROS_WARN("Velodyne poll() timeout");
                return result;
              }
            if ((fds[0].revents & POLLERR)
                || (fds[0].revents & POLLHUP)
                || (fds[0].revents & POLLNVAL)) // device error?
              {
                ROS_ERROR("poll() reports Velodyne error");
                return result;
              }
          } while ((fds[0].revents & POLLIN) == 0);

        // Read packets that should now be available from the socket.
        ssize_t nbytes = recvfrom(sockfd_, &buffer[i * packet_size],
                                  packet_size,  0, NULL, NULL);
        if ((size_t) nbytes == packet_size)
          {
            --result;
          }
        else
          {
            ROS_DEBUG_STREAM("incomplete Velodyne packet read: "
                             << nbytes << " bytes");
            --i;                        // rerun this loop iteration
          }
      }

    double time2 = ros::Time::now().toSec();

    // Average the times at which we begin and end reading.  Use that to
    // estimate when the scan occurred.
    *data_time = (time2 + time1) / 2.0;

    return result;
  }

  int InputSocket::vclose(void)
  {
    int rc = close(sockfd_);
    return rc;
  }

  ////////////////////////////////////////////////////////////////////////
  // InputPCAP class implementation
  ////////////////////////////////////////////////////////////////////////

  /** @brief constructor */
  InputPCAP::InputPCAP(ros::NodeHandle private_nh,
                       double packet_rate,
                       std::string filename,
                       bool read_once,
                       bool read_fast,
                       double repeat_delay):
    Input(private_nh),
    packet_rate_(packet_rate)
  {
    filename_ = filename;
    fp_ = NULL;  
    pcap_ = NULL;  
    empty_ = true;

    // get parameters from private node handle
    private_nh_.param("read_once", read_once_, read_once);
    private_nh_.param("read_fast", read_fast_, read_fast);
    private_nh_.param("repeat_delay", repeat_delay_, repeat_delay);

    if (read_once_)
      ROS_INFO("Read input file only once.");
    if (read_fast_)
      ROS_INFO("Read input file as quickly as possible.");
    if (repeat_delay_ > 0.0)
      ROS_INFO("Delay %.3f seconds before repeating input file.",
               repeat_delay_);
  }

  int InputPCAP::vopen(void) 
  {
    ROS_INFO("Opening PCAP file \"%s\"", filename_.c_str());
  
    /* Open the capture file */
    if ((pcap_ = pcap_open_offline(filename_.c_str(), errbuf_) ) == NULL)
      {
        /// \todo print error message, if pcap open fails
        ROS_FATAL("Error opening Velodyne socket dump file.");
        return -1;
      }
    return 0;
  }


  /** @brief Read Velodyne packets from PCAP dump file. */
  int InputPCAP::getPackets(uint8_t *buffer, int npacks, double *data_time)
  {
    struct pcap_pkthdr *header;
    const u_char *pkt_data;
    int result = npacks;

    for (int i = 0; i < npacks; ++i)
      {
        int res;
        if ((res = pcap_next_ex(pcap_, &header, &pkt_data)) >= 0)
          {
            // Keep the reader from blowing through the file.  The
            // actual device generates either 2600 (64E) or 1808 (32E)
            // packets per second at 600 RPM.
            if (read_fast_ == false)
              packet_rate_.sleep();
            
            memcpy(&buffer[i * packet_size], pkt_data+42, packet_size);
            *data_time = ros::Time::now().toSec();
            empty_ = false;
            --result;
            //ROS_DEBUG("%d Velodyne packets read", i);
          }
        else
          {
            if (empty_)                 // no data in file?
              {
                ROS_WARN("Error %d reading Velodyne packet: %s", 
                         res, pcap_geterr(pcap_));
                return result;
              }

            if (read_once_)
              {
                ROS_INFO("end of file reached -- done reading.");
                return -1;
              }

            if (repeat_delay_ > 0.0)
              {
                ROS_INFO("end of file reached -- delaying %.3f seconds.",
                         repeat_delay_);
                usleep(rint(repeat_delay_ * 1000000.0));
              }

            ROS_DEBUG("replaying Velodyne dump file");

            // I can't figure out how to rewind the file, because it
            // starts with some kind of header.  So, close the file
            // and reopen it with pcap.
            pcap_close(pcap_);
            pcap_ = pcap_open_offline(filename_.c_str(), errbuf_);
            empty_ = true;              // maybe the file disappeared?
            i = -1;                     // restart the loop
            result = npacks;
          }
      }

    return result;
  }

  int InputPCAP::vclose(void)
  {
    pcap_close(pcap_);
    return 0;
  }

} // velodyne namespace
