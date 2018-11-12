// SPDX-License-Identifier: GPL-2.0
#ifndef __B1_MAIN_H
#define __B1_MAIN_H

/**
 * DOC: Capability-based IPC for Linux
 *
 * Bus1 is a local IPC system, which provides a decentralized infrastructure to
 * share objects between local peers. The main building blocks are objects and
 * handles. Objects represent data of a local peer, while handles represent
 * descriptors that point to objects. Objects can be created and destroyed by
 * any peer, and they will always remain owned by their respective creator.
 * Handles, on the other hand, are used to refer to objects and can be passed
 * around with messages as auxiliary data. Whenever a handle is transferred,
 * the receiver will get its own handle allocated, pointing to the same object
 * as the original handle.
 *
 * Any peer can send messages directed at one of their handles. This will
 * transfer the message to the owner of the object the handle points to. If a
 * peer does not possess a handle to a given object, it will not be able to
 * send a message to that object. That is, handles provide exclusive access
 * management. Anyone that somehow acquired a handle to an object is privileged
 * to further send this handle to other peers. As such, access management is
 * transitive. Once a peer acquired a handle, it cannot be revoked again.
 * However, an object owner can, at any time, release an object. This will
 * effectively unbind all existing handles to that object on any peer,
 * notifying each one of the release.
 *
 * Unlike objects and handles, peers cannot be addressed directly. In fact,
 * peers are completely disconnected entities. A peer is merely an anchor of a
 * set of objects and handles, including an incoming message queue for any of
 * those. Whether multiple objects are all part of the same peer, or part of
 * different peers does not affect the remote view of those. Peers solely exist
 * as management entity and command dispatcher to local processes.
 *
 * The set of actors on a system is completely decentralized. There is no
 * global component involved that provides a central registry or discovery
 * mechanism. Furthermore, communication between peers only involves those
 * peers, and does not affect any other peer in any way. No global
 * communication lock is taken. However, any communication is still globally
 * ordered, including unicasts and multicasts.
 */

#include <linux/kernel.h>

#endif /* __B1_MAIN_H */
