%% Erlang SDK for the zed-pkg registry. Stdlib only (httpc + json + crypto);
%% maps mirror the JSON Schemas in zed-interfaces/schemas/.
-module(zed_pkg_client).

-export([
    new/1,
    new/2,
    with_token/2,
    get_package/3,
    get_version/4,
    search/2,
    claim_org/2,
    yank/5,
    download_artifact/2,
    download_artifact/3,
    publish/6,
    %% Exported for tests and reuse.
    package_path/2,
    version_path/3,
    artifact_path/1,
    yank_path/3,
    encode_segment/1,
    download_limit/1,
    allowed_download_url/2,
    verify_sha256/2,
    multipart_body/4
]).

-define(DEFAULT_REGISTRY_URL, <<"https://registry.zpkg.tech">>).

%% Bounds every request (connect + read), in milliseconds.
-define(DEFAULT_TIMEOUT, 30000).

%% Hard ceiling on artifact downloads, matching the server's
%% MAX_ARTIFACT_BYTES default (100 MiB); plus the slack added to a version's
%% declared size.
-define(MAX_ARTIFACT_BYTES, 104857600).
-define(DOWNLOAD_SLACK, 1048576).

%%--------------------------------------------------------------------
%% Construction
%%--------------------------------------------------------------------

new(BaseUrl) ->
    new(BaseUrl, ?DEFAULT_TIMEOUT).

new(BaseUrl, Timeout) when is_integer(Timeout), Timeout > 0 ->
    {ok, #{
        base => normalize_base(text(BaseUrl)),
        token => undefined,
        timeout => Timeout
    }};
new(_BaseUrl, _Timeout) ->
    {error, {invalid_configuration, <<"timeout must be a positive integer">>}}.

%% Attach a bearer token used for authenticated calls (claim_org, yank,
%% publish). Never sent on artifact downloads.
with_token(Client, Token) when is_map(Client) ->
    Client#{token => optional_text(Token)}.

normalize_base(Base) ->
    case binary:last(Base) of
        $/ -> normalize_base(binary:part(Base, 0, byte_size(Base) - 1));
        _ -> Base
    end.

%%--------------------------------------------------------------------
%% Paths (percent-encode every segment so opaque version tags cannot break
%% out of their URL segment)
%%--------------------------------------------------------------------

encode_segment(Segment) ->
    iolist_to_binary(uri_string:quote(text(Segment))).

package_path(Org, Name) ->
    <<"/v1/packages/", (encode_segment(Org))/binary, "/", (encode_segment(Name))/binary>>.

version_path(Org, Name, Version) ->
    <<(package_path(Org, Name))/binary, "/versions/", (encode_segment(Version))/binary>>.

artifact_path(Sha256) ->
    <<"/v1/artifacts/", (encode_segment(Sha256))/binary>>.

yank_path(Org, Name, Version) ->
    <<(version_path(Org, Name, Version))/binary, "/yank">>.

%%--------------------------------------------------------------------
%% Download policy and integrity
%%--------------------------------------------------------------------

%% The declared size (when sane) plus slack, capped by the ceiling.
download_limit(Size) when is_integer(Size), Size > 0 ->
    min(Size + ?DOWNLOAD_SLACK, ?MAX_ARTIFACT_BYTES);
download_limit(_Size) ->
    ?MAX_ARTIFACT_BYTES.

%% Enforce the download-url scheme policy: https is always allowed; http only
%% for loopback hosts or when the registry base is itself http. A malicious
%% registry response must not redirect fetches to plaintext or unexpected
%% hosts.
allowed_download_url(Raw, Base) ->
    RawText = text(Raw),
    case uri_string:parse(RawText) of
        #{scheme := <<"https">>} ->
            {ok, RawText};
        #{scheme := <<"http">>, host := Host} ->
            case is_loopback(Host) orelse is_http_base(text(Base)) of
                true -> {ok, RawText};
                false ->
                    {error,
                        {insecure_download_url,
                            <<"refusing artifact download over `http` from ", RawText/binary,
                                " (https required for non-local registries)">>}}
            end;
        _ ->
            {error,
                {insecure_download_url,
                    <<"refusing artifact download from ", RawText/binary,
                        " (https required for non-local registries)">>}}
    end.

is_http_base(<<"http://", _/binary>>) -> true;
is_http_base(_) -> false.

is_loopback(<<"localhost">>) ->
    true;
is_loopback(Host) when is_binary(Host) ->
    case inet:parse_address(binary_to_list(Host)) of
        {ok, Address} ->
            case Address of
                {127, _, _, _} -> true;
                {0, 0, 0, 0, 0, 0, 0, 1} -> true;
                _ -> false
            end;
        {error, _} ->
            false
    end.

%% Verify a binary against an expected lowercase hex sha256.
verify_sha256(Bytes, Expected) ->
    Actual = binary:encode_hex(crypto:hash(sha256, Bytes), lowercase),
    case Actual =:= text(Expected) of
        true -> ok;
        false -> {error, {sha256_mismatch, #{expected => text(Expected), actual => Actual}}}
    end.

%%--------------------------------------------------------------------
%% Operations
%%--------------------------------------------------------------------

%% GET /v1/packages/{org}/{name} — package metadata + version list.
get_package(Client, Org, Name) ->
    request_json(Client, get, package_path(Org, Name), undefined, false).

%% GET /v1/packages/{org}/{name}/versions/{version}.
get_version(Client, Org, Name, Version) ->
    request_json(Client, get, version_path(Org, Name, Version), undefined, false).

%% GET /v1/search?q=.
search(Client, Query) ->
    Path = <<"/v1/search?q=", (encode_segment(Query))/binary>>,
    request_json(Client, get, Path, undefined, false).

%% POST /v1/orgs (bearer token).
claim_org(Client, Slug) ->
    request_json(Client, post, <<"/v1/orgs">>, #{<<"slug">> => text(Slug)}, true).

%% POST .../versions/{version}/yank — yank (true) or restore (false) a
%% published version. Requires a bearer token with publish rights.
yank(Client, Org, Name, Version, Yanked) when is_boolean(Yanked) ->
    request_json(Client, post, yank_path(Org, Name, Version), #{<<"yanked">> => Yanked}, true).

%% Download an artifact, verify its sha256, and return the bytes. `Version`
%% is the (decoded) version-metadata map returned by get_version/4.
download_artifact(Client, Version) when is_map(Version) ->
    DownloadUrl = text(maps:get(<<"download_url">>, Version, <<>>)),
    Sha256 = text(maps:get(<<"sha256">>, Version, <<>>)),
    Size = maps:get(<<"size">>, Version, 0),
    Base = maps:get(base, Client),
    %% An absolute url (any scheme) must clear the scheme/host policy; a bare
    %% path is resolved against the trusted registry base.
    UrlResult =
        case binary:match(DownloadUrl, <<"://">>) of
            nomatch -> {ok, <<Base/binary, (artifact_path(Sha256))/binary>>};
            _ -> allowed_download_url(DownloadUrl, Base)
        end,
    case UrlResult of
        {ok, Url} ->
            %% Deliberately no authorization header: download_url may point at
            %% a third-party host (e.g. a presigned S3/R2 url) and the bearer
            %% token must not leak there.
            case http_request(Client, get, Url, [], undefined) of
                {ok, Status, Body} when Status >= 200, Status < 300 ->
                    Limit = download_limit(Size),
                    case byte_size(Body) > Limit of
                        true ->
                            {error, {artifact_too_large, Limit}};
                        false ->
                            case verify_sha256(Body, Sha256) of
                                ok -> {ok, Body};
                                Error -> Error
                            end
                    end;
                {ok, Status, Body} ->
                    {error, decode_api_error(Status, Body)};
                Error ->
                    Error
            end;
        Error ->
            Error
    end.

%% Download to DestPath instead of returning the bytes.
download_artifact(Client, Version, DestPath) ->
    case download_artifact(Client, Version) of
        {ok, Body} ->
            case filelib:ensure_dir(DestPath) of
                ok -> file:write_file(DestPath, Body);
                Error -> Error
            end;
        Error ->
            Error
    end.

%% Publish: multipart `meta` (PublishMeta JSON) + `artifact` bytes via
%% PUT /v1/packages/{org}/{name}/versions/{version}. Requires a bearer token.
%% Org/Name/Version must match meta.manifest.package.
publish(Client, Org, Name, Version, MetaJson, Artifact) when is_binary(Artifact) ->
    Boundary = <<"zedpkg", (binary:encode_hex(crypto:strong_rand_bytes(16), lowercase))/binary>>,
    Filename = iolist_to_binary([text(Org), "-", text(Name), "-", text(Version), ".tar.gz"]),
    Body = multipart_body(Boundary, text(MetaJson), Filename, Artifact),
    ContentType = <<"multipart/form-data; boundary=", Boundary/binary>>,
    Path = version_path(Org, Name, Version),
    Url = <<(maps:get(base, Client))/binary, Path/binary>>,
    Headers = auth_headers(Client, true),
    case http_request(Client, put, Url, Headers, {ContentType, Body}) of
        {ok, Status, ResponseBody} when Status >= 200, Status < 300 ->
            decode_json(ResponseBody);
        {ok, Status, ResponseBody} ->
            {error, decode_api_error(Status, ResponseBody)};
        Error ->
            Error
    end.

%% Build the multipart/form-data body for a publish: a `meta` field carrying
%% the PublishMeta JSON and an `artifact` file part.
multipart_body(Boundary, MetaJson, Filename, Artifact) ->
    iolist_to_binary([
        "--", Boundary, "\r\n",
        "content-disposition: form-data; name=\"meta\"\r\n\r\n",
        MetaJson, "\r\n",
        "--", Boundary, "\r\n",
        "content-disposition: form-data; name=\"artifact\"; filename=\"", Filename, "\"\r\n",
        "content-type: application/octet-stream\r\n\r\n",
        Artifact, "\r\n",
        "--", Boundary, "--\r\n"
    ]).

%%--------------------------------------------------------------------
%% Transport
%%--------------------------------------------------------------------

request_json(Client, Method, Path, Body, Authorized) ->
    Url = <<(maps:get(base, Client))/binary, Path/binary>>,
    Headers = auth_headers(Client, Authorized),
    Payload =
        case Body of
            undefined -> undefined;
            _ -> {<<"application/json">>, iolist_to_binary(json:encode(Body))}
        end,
    case http_request(Client, Method, Url, Headers, Payload) of
        {ok, Status, ResponseBody} when Status >= 200, Status < 300 ->
            decode_json(ResponseBody);
        {ok, Status, ResponseBody} ->
            {error, decode_api_error(Status, ResponseBody)};
        Error ->
            Error
    end.

auth_headers(Client, true) ->
    case maps:get(token, Client, undefined) of
        undefined -> [];
        Token -> [{"authorization", "Bearer " ++ binary_to_list(Token)}]
    end;
auth_headers(_Client, false) ->
    [].

http_request(Client, Method, Url, Headers0, Payload) ->
    _ = application:ensure_all_started([inets, ssl]),
    Timeout = maps:get(timeout, Client, ?DEFAULT_TIMEOUT),
    Headers = [{"accept", "application/json"} | Headers0],
    Request =
        case Payload of
            undefined ->
                {binary_to_list(Url), Headers};
            {ContentType, RequestBody} ->
                {binary_to_list(Url), Headers, binary_to_list(ContentType), RequestBody}
        end,
    HttpOptions = [
        {timeout, Timeout},
        {connect_timeout, Timeout},
        {autoredirect, false}
    ],
    Options = [{body_format, binary}],
    case httpc:request(Method, Request, HttpOptions, Options) of
        {ok, {{_Version, Status, _Reason}, _ResponseHeaders, ResponseBody}} ->
            {ok, Status, ResponseBody};
        {error, Reason} ->
            {error, {transport, Reason}}
    end.

%%--------------------------------------------------------------------
%% Decoding
%%--------------------------------------------------------------------

decode_json(<<>>) ->
    {error, {protocol, empty_json_body}};
decode_json(Body) ->
    try
        {ok, json:decode(Body)}
    catch
        Class:Reason -> {error, {protocol, {invalid_json, Class, Reason}}}
    end.

%% Map an error-response body to {api_error, Status, Code, Message}, keeping
%% the "unknown" code when the body is not ApiError JSON.
decode_api_error(Status, Body) ->
    try json:decode(Body) of
        #{<<"code">> := Code, <<"message">> := Message} ->
            {api_error, Status, Code, Message};
        _ ->
            {api_error, Status, <<"unknown">>, Body}
    catch
        _:_ -> {api_error, Status, <<"unknown">>, Body}
    end.

%%--------------------------------------------------------------------
%% Helpers
%%--------------------------------------------------------------------

text(Value) when is_binary(Value) -> Value;
text(Value) when is_list(Value) -> unicode:characters_to_binary(Value);
text(Value) when is_atom(Value) -> atom_to_binary(Value, utf8).

optional_text(undefined) -> undefined;
optional_text(<<>>) -> undefined;
optional_text("") -> undefined;
optional_text(Value) -> text(Value).
