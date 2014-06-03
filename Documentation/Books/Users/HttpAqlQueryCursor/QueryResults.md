<a name="retrieving_query_results"></a>
# Retrieving query results

Select queries are executed on-the-fly on the server and the result
set will be returned back to the client.

There are two ways the client can get the result set from the server:

- In a single roundtrip
- Using a cursor

<a name="single_roundtrip"></a>
### Single roundtrip

The server will only transfer a certain number of result documents back to the
client in one roundtrip. This number is controllable by the client by setting
the `batchSize` attribute when issuing the query.

If the complete result can be transferred to the client in one go, the client
does not need to issue any further request. The client can check whether it has
retrieved the complete result set by checking the `hasMore` attribute of the
result set. If it is set to `false`, then the client has fetched the complete
result set from the server. In this case no server side cursor will be created.

__Examples__


@verbinclude api-cursor-create-for-limit-return-single

<a name="using_a_cursor"></a>
### Using a Cursor

If the result set contains more documents than should be transferred in a single
roundtrip (i.e. as set via the `batchSize` attribute), the server will return
the first few documents and create a temporary cursor. The cursor identifier
will also be returned to the client. The server will put the cursor identifier
in the `id` attribute of the response object. Furthermore, the `hasMore`
attribute of the response object will be set to `true`. This is an indication
for the client that there are additional results to fetch from the server.

__Examples__


Create and extract first batch:

@verbinclude api-cursor-create-for-limit-return

Extract next batch, still have more:

@verbinclude api-cursor-create-for-limit-return-cont

Extract next batch, done:

@verbinclude api-cursor-create-for-limit-return-cont2

Do not do this:

@verbinclude api-cursor-create-for-limit-return-cont3