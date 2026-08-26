# KVS PutMedia

The Amazon Kinesis Video Streams Producer REST client: the `DescribeStream`,
`CreateStream` and `GetDataEndpoint` control-plane calls plus the `PutMedia`
data-plane request, with SigV4 request signing.

Derived from
[amazon-kinesis-video-streams-producer-embedded-c](https://github.com/aws-samples/amazon-kinesis-video-streams-producer-embedded-c)
(Apache-2.0). MKV generation is not part of this component — it lives in
[`kvs_producer`](../kvs_producer/), which this one depends on.
