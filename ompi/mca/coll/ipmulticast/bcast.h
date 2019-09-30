
#ifndef MCA_COLL_IPMULTICAST_EXPORT_H
#define MCA_COLL_IPMULTICAST_EXPORT_H

#include "ompi_config.h"

#include "mpi.h"
#include "ompi/constants.h"
#include "ompi/datatype/ompi_datatype.h"
#include "ompi/datatype/ompi_datatype_internal.h"
#include "ompi/op/op.h"
#include "ompi/mca/mca.h"
#include "opal/datatype/opal_convertor.h"
#include "ompi/mca/coll/coll.h"
#include "ompi/request/request.h"
#include "ompi/communicator/communicator.h"
#include "ompi/mca/coll/base/base.h"
#include "ompi/datatype/ompi_datatype.h"

#include "ompi/mca/coll/portals4/coll_portals4.h"

BEGIN_C_DECLS

#define IP_MULTICAST_PORT 12441
#define IP_MULTICAST_ADDR "224.0.0.7"

int ompi_coll_ipmulticast_bcast(void *buff, int count,
        struct ompi_datatype_t *datatype, int root,
        struct ompi_communicator_t *comm,mca_coll_base_module_t *module) {
    printf("Calling custom bcast\n");

    bool isroot = ompi_comm_rank(comm) == root;

    int fd;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(IP_MULTICAST_PORT);
    if (!isroot) {
        bool yes = true;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        addr.sin_addr.s_addr = htonl(INADDR_ANY); 

        if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
            perror("bind");
            return 1;
        }
        // use setsockopt() to request that the kernel join a multicast group
        //
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(IP_MULTICAST_ADDR);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*) &mreq, sizeof(mreq)) < 0) {
            perror("setsockopt");
            return 1;
        }
    }

    ompi_coll_portals4_barrier_intra(comm, module);

    ssize_t nbytes;
    if (isroot) {
        addr.sin_addr.s_addr = inet_addr(IP_MULTICAST_ADDR);

        nbytes = sendto(fd, buff, count, 0, (struct sockaddr*) &addr, sizeof(addr));
		if (nbytes < 0)
			perror("sendto");
    }
	else {
		int addrlen = sizeof(addr);
		// count is not the right value to pass
		nbytes = recvfrom(fd,
				buff, count,
				0, (struct sockaddr *) &addr, &addrlen);
		if (nbytes < 0)
			perror("recvfrom");
	}
    printf("Root? %d sent/received %zd bytes\n", (int) isroot, nbytes);
    close(fd);
    return (OMPI_SUCCESS);
}

END_C_DECLS
#endif
