using System;
using System.Threading;
using System.Threading.Tasks;
using MQTTnet;
using MQTTnet.Client;
using MQTTnet.Formatter;

/// <summary>
/// Connects to the RoboMaster MQTT server using the robot ID as the client ID.
/// MQTTnet handles the handshake and keep-alive; no topics are subscribed or published.
/// </summary>
public sealed class MqttConnector : IDisposable, IAsyncDisposable
{
    public const string DefaultHost = "192.168.12.1";
    public const int DefaultPort = 3333;

    private readonly IMqttClient _client;
    private readonly MqttClientOptions _options;
    private bool _disposed;

    public MqttConnector(string clientId, string host = DefaultHost, int port = DefaultPort)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(clientId);
        ArgumentException.ThrowIfNullOrWhiteSpace(host);
        if (port < 1 || port > 65535) throw new ArgumentOutOfRangeException(nameof(port));

        _options = new MqttClientOptionsBuilder()
            .WithClientId(clientId)
            .WithTcpServer(host, port)
            .WithProtocolVersion(MqttProtocolVersion.V311)
            .WithCleanSession()
            .WithKeepAlivePeriod(TimeSpan.FromSeconds(15))
            .WithTimeout(TimeSpan.FromSeconds(10))
            .Build();
        _client = new MqttFactory().CreateMqttClient();
    }

    public bool IsConnected => !_disposed && _client.IsConnected;

    /// <summary>Completes after CONNACK succeeds; cancellation and connection errors propagate to the caller.</summary>
    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        cancellationToken.ThrowIfCancellationRequested();
        // MQTTnet 4 skips its connection timeout when a cancellable token is supplied.
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(_options.Timeout);
        try
        {
            await _client.ConnectAsync(_options, timeout.Token).ConfigureAwait(false);
        }
        catch (Exception error) when (cancellationToken.IsCancellationRequested)
        {
            // Authentication errors in MQTTnet can wrap OperationCanceledException.
            throw new OperationCanceledException("MQTT connection cancelled.", error, cancellationToken);
        }
        catch (Exception error) when (timeout.IsCancellationRequested)
        {
            throw new TimeoutException("MQTT connection timed out after 10 seconds.", error);
        }
    }

    public Task DisconnectAsync(CancellationToken cancellationToken = default) =>
        _client.DisconnectAsync(new MqttClientDisconnectOptions(), cancellationToken);

    /// <summary>Closes the transport immediately, including an in-progress connection.</summary>
    public void Dispose()
    {
        _disposed = true;
        _client.Dispose();
    }

    /// <summary>Sends DISCONNECT before releasing an established connection.</summary>
    public async ValueTask DisposeAsync()
    {
        try
        {
            if (IsConnected) await DisconnectAsync().ConfigureAwait(false);
        }
        finally
        {
            Dispose();
        }
    }
}
