<a name="replication_limitations"></a>
# Replication Limitations

The replication in ArangoDB 1.4-alpha has a few limitations still. Some of these 
limitations may be removed in later versions of ArangoDB:

- the event log of the master database is currently written directly after a write operation
  is carried out in the master database. In case the master crashes between having executed the
  write operation and having it written into the event log, the write operation may
  have been executed on the master, but may be lost for replication and not be applied
  on any slaves.
- there is no feedback from the slaves to the master. If a slave cannot apply an event
  it got from the master, the master will have a different state of data. In this 
  case, the replication applier on the slave will stop and report an error. Administrators
  can then either "fix" the problem or re-sync the data from the master to the slave
  and start the applier again.
- the replication is an asynchronous master-slave replication. There is currently no
  way to use it as a synchronous replication, or a multi-master replication.
- at the moment it is assumed that only the replication applier executes write 
  operations on a slave. ArangoDB currently does not prevent users from carrying out
  their own write operations on slaves, though this might lead to undefined behavior
  and the replication applier stopping.
- the replication logger will log write operations for all user-defined collections and
  only some system collections. Write operations for the following system collections 
  are excluded intentionally: `_trx`, `_replication`, `_users`, `_aal`, `_fishbowl`, 
  `_modules` and `_routing`. Write operations for the following system collections
  will be logged: `_aqlfunctions`, `_graphs`.
- Foxx applications consist of database entries and application scripts in the file system.
  The file system parts of Foxx applications are not tracked anywhere and thus not 
  replicated in current versions of ArangoDB. To replicate a Foxx application, it is
  required to copy the application to the remote server and install it there using the
  `foxx-manager` utility. 
- master servers do not know which slaves are or will be connected to them. All servers
  in a replication setup are currently only loosely coupled. There currently is no way 
  for a client to query which servers are present in a replication.
- failover must currently be handled by clients or client APIs.
- there currently is one replication logger and one replication applier per ArangoDB 
  database. It is thus not possible to have a slave apply the events logs from 
  multiple masters.
- replication is set up on a per-database level. When using ArangoDB with multiple
  databases, replication must be configured individually for each database.
- the replication applier is single-threaded, but write operations on the master may
  be executed in parallel if they affect different collections. Thus the replication
  applier might not be able to catch up with a very powerful and loaded master.
- replication is only supported between ArangoDB 1.4 masters and 1.4 slaves. It is
  currently not possible to replicate from/to other ArangoDB versions.
- a replication applier cannot apply data from itself.
- when doing the initial synchronization from a master to a slave using the `sync`
  method, collections are not prevented from being dropped in-between. That means that
  when a client fetches the data for collections C1, C2, and C3 (in this order), it 
  will start with collection C1. Any other client may come along and drop collection
  C3 in the meantime. When the client reaches collection C3, it will not find this
  collection anymore, and the initial sync will fail. This will be fixed until the
  1.4 stable release.
