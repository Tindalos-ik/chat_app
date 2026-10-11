"""Actual isolated HTTP/TCP/gRPC group and avatar smoke; never targets developer data.
Run with --runtime-dir pointing at the prepared private fixture directory. Credentials
are read privately from its configs; output contains assertion labels, never credentials.
Requires grpcio/grpcio-tools, Pillow, and an explicit MySQL client binary.
"""
import argparse, base64, configparser, hashlib, io, json, os, random, secrets, socket, struct, subprocess, sys, time, uuid, zlib
from pathlib import Path
from urllib.request import Request, urlopen
from PIL import Image


def require(value, label):
    if not value:
        raise AssertionError(label)


class Frames:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=8)
        self.socket.settimeout(8)
        self.events = []

    def close(self):
        self.socket.close()

    def exact(self, size):
        result = b''
        while len(result) < size:
            chunk = self.socket.recv(size - len(result))
            require(chunk, 'connection closed before complete frame')
            result += chunk
        return result

    def send(self, message_id, value):
        body = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        require(len(body) <= 2048, 'smoke request exceeds protocol byte limit')
        self.socket.sendall(struct.pack('!HH', message_id, len(body)) + body)

    def receive(self):
        message_id, size = struct.unpack('!HH', self.exact(4))
        require(size <= 4096, 'actual outgoing frame exceeds 4096')
        return message_id, json.loads(self.exact(size))

    def exchange(self, message_id, body, correlation=True):
        request = dict(body)
        if correlation:
            request.setdefault('request_id', uuid.uuid4().hex)
        self.send(message_id, request)
        while True:
            received, value = self.receive()
            if received == message_id + 1 and (not correlation or value.get('request_id') == request['request_id']):
                if 'operation_id' in request:
                    require(value.get('operation_id') == request['operation_id'], 'mutation error/success operation echo')
                return value
            self.events.append((received, value))

    def event(self, uid, kind, thread):
        deadline = time.monotonic() + 8
        while True:
            for index, (message_id, value) in enumerate(self.events):
                if message_id == 1081 and value.get('event_type') == kind and int(value.get('thread_id', 0)) == thread:
                    self.events.pop(index)
                    require(value.get('uid') == uid and value.get('type') == 'group', 'actual recipient event envelope')
                    return value
            require(time.monotonic() < deadline, 'expected actual typed notification missing')
            self.events.append(self.receive())

    def notification(self, message_id, predicate):
        deadline=time.monotonic()+8
        while True:
            for index,(received,value) in enumerate(self.events):
                if received == message_id and predicate(value):
                    self.events.pop(index)
                    return value
            require(time.monotonic() < deadline,'expected private notification missing')
            self.events.append(self.receive())

class ResourceFrames(Frames):
    """ResourceServer preserves its uint16+uint32 six-byte private protocol."""
    def send(self, message_id, value):
        body=json.dumps(value,ensure_ascii=False,separators=(',', ':')).encode('utf-8')
        require(len(body) <= 65536,'resource request exceeds 64KiB framing limit')
        self.socket.sendall(struct.pack('!HI',message_id,len(body))+body)

    def receive(self):
        message_id,size=struct.unpack('!HI',self.exact(6))
        require(size <= 65536,'resource response exceeds 64KiB framing limit')
        return message_id,json.loads(self.exact(size))

class Smoke:
    def __init__(self, directory, mysql):
        self.directory, self.mysql = directory, mysql
        self.config = configparser.ConfigParser(interpolation=None)
        self.config.read(directory / 'ResourceServer' / 'config.ini', encoding='utf-8')
        self.schema = self.config['Mysql']['schema']
        require(self.schema.startswith('chat_app_test_') and self.schema.replace('_', '').isalnum(), 'explicit isolated schema required')
        self.connections, self.groups, self.resources = [], [], []
        self.issued_tokens = []
        self.management_only = False
        self.password = secrets.token_hex(12)
        self.wire_password = ''.join(chr(ord(c) ^ len(self.password)) for c in self.password)
        self.actors = {}
        generated = directory / 'generated-python-rpc'
        require((generated / 'message_pb2.py').exists(), 'real generated RPC client required')
        sys.path.insert(0, str(generated))
        import grpc, message_pb2, message_pb2_grpc
        self.pb = message_pb2
        self.rpc = message_pb2_grpc.ResourceServiceStub(grpc.insecure_channel('127.0.0.1:35057'))
        self.service_key = self.config['ResourceServer']['service_key']
        self.keyless_process = None

    def keyless_verification(self, actor, thread, resource):
        """Run a real separately configured service with no caller credential."""
        directory = self.directory / 'ResourceServerNoKey'
        directory.mkdir(exist_ok=True)
        config = configparser.ConfigParser(interpolation=None)
        config.read(self.directory / 'ResourceServer' / 'config.ini', encoding='utf-8')
        config['ResourceServer']['port'] = '39092'
        config['ResourceServer']['rpcport'] = '35058'
        config['ResourceServer']['service_key'] = ''
        base = Path(config['ResourceServer']['file_path'])
        if not base.is_absolute():
            base = (self.directory / 'ResourceServer' / base).resolve()
        config['ResourceServer']['file_path'] = str(base)
        with (directory / 'config.ini').open('w', encoding='utf-8') as output:
            config.write(output)
        executable = Path(__file__).resolve().parents[1] / 'build' / 'ResourceServer' / 'Debug' / 'ResourceServer.exe'
        require(executable.exists(), 'real keyless ResourceServer executable exists')
        with (directory / 'runtime.log').open('wb') as output:
            self.keyless_process = subprocess.Popen([str(executable)], cwd=directory, stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0)
        deadline = time.monotonic() + 20
        while True:
            require(self.keyless_process.poll() is None, 'keyless fixture service exited before readiness')
            try:
                with socket.create_connection(('127.0.0.1', 35058), timeout=0.5):
                    break
            except OSError:
                require(time.monotonic() < deadline, 'keyless fixture service readiness timeout')
                time.sleep(0.1)
        import grpc, message_pb2_grpc
        rpc = message_pb2_grpc.ResourceServiceStub(grpc.insecure_channel('127.0.0.1:35058'))
        user = self.actors[actor]
        result = rpc.VerifyGroupAvatar(self.pb.GroupAvatarVerifyReq(uid=user['uid'], token=user['token'], thread_id=thread, resource_id=resource), metadata=[('x-resource-service-key', self.service_key)], timeout=8)
        require(result.error != 0, 'absent configured service secret fails closed despite valid token and supplied caller key')

    def sql(self, statement):
        process = subprocess.run([self.mysql, '--no-defaults', '--protocol=tcp', '-h127.0.0.1', '-P33316', '-uroot', '--default-character-set=utf8mb4', '--batch', '--skip-column-names', self.schema], input=statement, capture_output=True, text=True, encoding='utf-8')
        require(process.returncode == 0, 'isolated fixture SQL failed')
        return process.stdout.strip()

    def seed(self):
        for i, name in enumerate('abcdef'):
            uid = 1900710001 + i
            existing = self.sql(f'SELECT name FROM user WHERE uid={uid}')
            require(not existing or existing == 'backend_smoke_' + name, 'fixture UID occupied by unrelated account')
            self.sql(f"INSERT INTO user(uid,name,email,pwd,nick) VALUES({uid},'backend_smoke_{name}','backend_smoke_{name}@isolated.invalid',CONVERT(0x{self.wire_password.encode().hex()} USING utf8mb4),'Backend {name}') ON DUPLICATE KEY UPDATE pwd=VALUES(pwd)")
            self.actors[name] = {'uid': uid, 'name': 'backend_smoke_' + name}
        for first, second in [('a','b'),('a','c'),('a','d'),('a','e'),('b','d'),('b','c')]:
            a, b = self.actors[first]['uid'], self.actors[second]['uid']
            self.sql(f"INSERT IGNORE INTO friend(self_id,friend_id,back) VALUES({a},{b},''),({b},{a},'')")
        for i, (name, actor) in enumerate(self.actors.items()):
            request = Request('http://127.0.0.1:38080/user_login', json.dumps({'user': actor['name'], 'passwd': self.wire_password}).encode(), {'Content-Type': 'application/json'})
            with urlopen(request, timeout=10) as response:
                result = json.load(response)
            require(result.get('error') == 0, 'real HTTP fixture login')
            actor['token'] = result['token']
            self.issued_tokens.append(actor['token'])
            actor['connection'] = Frames(38090 if i % 2 == 0 else 38091)
            self.connections.append(actor['connection'])
            require(actor['connection'].exchange(1005, {'uid': actor['uid'], 'token': actor['token']}, False).get('error') == 0, 'real TCP fixture login')

    def mutate(self, actor, message, thread=None, **fields):
        request = {'operation_id': uuid.uuid4().hex, **fields}
        if thread is not None:
            request['thread_id'] = str(thread)
        return self.actors[actor]['connection'].exchange(message, request)

    def members(self, actor, thread):
        return self.actors[actor]['connection'].exchange(1051, {'thread_id': str(thread), 'after_uid': '0', 'page_size': 100})

    def version(self, actor, target, thread):
        result = self.members(actor, thread)
        require(result.get('error') == 0, 'current member listing')
        return next(item['membership_version'] for item in result['members'] if item['uid'] == self.actors[target]['uid'])

    def create(self, actor, name):
        result = self.mutate(actor, 1043, name=name)
        require(result.get('error') == 0, 'real group create')
        thread = int(result['thread_id']); self.groups.append(thread)
        return thread

    def resource(self, message, request):
        connection = ResourceFrames(39090)
        try:
            response = connection.exchange(message, request, False)
            if request.get('purpose') == 'group_avatar':
                for key in ['purpose', 'uid', 'thread_id', 'resource_id']:
                    require(response.get(key) == request.get(key), 'group task success/error identity echo ' + key)
                expected_task=request.get('task_id',request.get('upload_id',request.get('resource_id')))
                require(response.get('task_id') == expected_task,'group task success/error task_id echo')
                if 'upload_id' in request:
                    require(response.get('upload_id') == request['upload_id'],'group upload identity echo')
                require('token' not in response,'group response excludes credential')
            return response
        finally:
            connection.close()

    def upload_request(self, actor, thread, resource, data):
        user = self.actors[actor]
        return {'purpose': 'group_avatar', 'uid': user['uid'], 'token': user['token'], 'thread_id': str(thread), 'resource_id': resource, 'upload_id': resource, 'md5': hashlib.md5(data).hexdigest(), 'name': 'avatar.png', 'total_size': len(data), 'chunk_size': 512}

    def upload(self, actor, thread, data):
        identity = str(uuid.uuid4()); self.resources.append(identity)
        request = self.upload_request(actor, thread, identity, data)
        require(self.resource(1005, request).get('error') == 0, 'authenticated upload sync')
        for offset in range(0, len(data), 512):
            chunk = data[offset:offset + 512]
            reply = self.resource(1003, {**request, 'offset': offset, 'data': base64.b64encode(chunk).decode(), 'is_last': offset + len(chunk) == len(data)})
            require(reply.get('error') == 0, 'authenticated upload chunk')
        require(reply.get('completed') and reply.get('resource_id') == identity and 'resource_url' not in reply, 'completed group resource identity without path')
        return identity, request

    def verify(self, actor, thread, resource, metadata=True, token=None, key=None):
        user = self.actors[actor]
        request = self.pb.GroupAvatarVerifyReq(uid=user['uid'], token=token or user['token'], thread_id=thread, resource_id=resource)
        headers = [('x-resource-service-key', key if key is not None else self.service_key)] if metadata else []
        return self.rpc.VerifyGroupAvatar(request, metadata=headers, timeout=8)

    def download(self, actor, thread, resource, offset=0):
        user = self.actors[actor]
        return self.resource(1007, {'purpose': 'group_avatar', 'uid': user['uid'], 'token': user['token'], 'thread_id': str(thread), 'resource_id': resource, 'task_id':resource, 'offset': offset, 'chunk_size': 32})

    def private_resource_regression(self, image_resource, image_data):
        """Exercise original private image/file send, download, receipt and recovery."""
        sender, receiver = self.actors['c'], self.actors['b']
        image_uuid=uuid.uuid4().hex
        image_item={'msgid':image_uuid,'resource_id':image_resource,'name':'untrusted','mime_type':'untrusted','file_size':1,'width':1,'height':1}
        image=sender['connection'].exchange(1033,{'fromuid':sender['uid'],'touid':receiver['uid'],'imageArray':[image_item]},False)
        require(image.get('error') == 0 and image.get('persisted'),'actual private image persisted after group leave/dissolve')
        canonical=image['imageArray'][0]
        require(canonical['resource_id'] == image_resource and canonical['width'] == 16 and canonical['file_size'] == len(image_data),'private image trusted metadata unchanged')
        thread=int(image['thread_id']);image_id=int(canonical['message_id'])
        pushed=receiver['connection'].notification(1035,lambda value:any(item.get('msgid') == image_uuid for item in value.get('imageArray',[])))
        require(int(pushed['imageArray'][0]['message_id']) == image_id,'cross-server private image identical durable ID')
        file_data=b'Original private file regression, not group content.\\n'*4
        file_resource=str(uuid.uuid4());self.resources.append(file_resource)
        upload={'upload_id':file_resource,'name':'private.txt','md5':hashlib.md5(file_data).hexdigest(),'total_size':len(file_data),'chunk_size':512}
        require(self.resource(1005,upload).get('error') == 0,'original anonymous private file sync')
        completed=self.resource(1003,{**upload,'offset':0,'data':base64.b64encode(file_data).decode(),'is_last':True})
        require(completed.get('error') == 0 and completed.get('completed') and isinstance(completed.get('resource_url'),str),'legacy resource_url behavior preserved')
        file_uuid=uuid.uuid4().hex
        file=sender['connection'].exchange(1040,{'fromuid':sender['uid'],'touid':receiver['uid'],'fileArray':[{'msgid':file_uuid,'resource_id':file_resource,'file_size':999999,'mime_type':'untrusted'}]},False)
        require(file.get('error') == 0 and file.get('persisted') and int(file['thread_id']) == thread,'actual private file persisted same private thread')
        file_item=file['fileArray'][0];file_id=int(file_item['message_id'])
        require(file_item['file_size'] == len(file_data) and file_item['mime_type'] == 'text/plain','private file metadata trusted unchanged')
        pushed=receiver['connection'].notification(1042,lambda value:any(item.get('msgid') == file_uuid for item in value.get('fileArray',[])))
        require(int(pushed['fileArray'][0]['message_id']) == file_id,'cross-server private file durable ID')
        for resource,data in [(image_resource,image_data),(file_resource,file_data)]:
            received=b''
            for offset in range(0,len(data),32):
                reply=self.resource(1007,{'uid':receiver['uid'],'token':receiver['token'],'thread_id':thread,'resource_id':resource,'offset':offset,'chunk_size':32})
                require(reply.get('error') == 0,'original private authorized chunk download')
                received+=base64.b64decode(reply['data'])
            require(received == data,'original private complete download hash/bytes match')
            require(self.resource(1007,{'uid':self.actors['a']['uid'],'token':self.actors['a']['token'],'thread_id':thread,'resource_id':resource,'offset':0,'chunk_size':32}).get('error') != 0,'original private outsider download denied')
        receiver['connection'].send(1036,{'thread_id':str(thread),'message_ids':[str(image_id),str(file_id)]})
        displayed=sender['connection'].notification(1037,lambda value:int(value.get('thread_id',0)) == thread)
        require(set(map(int,displayed['message_ids'])) == {image_id,file_id},'original image/file actual display receipt')
        receiver['connection'].send(1038,{'thread_id':str(thread),'read_through_message_id':str(file_id)})
        read=sender['connection'].notification(1039,lambda value:int(value.get('thread_id',0)) == thread)
        require(int(read['read_through_message_id']) == file_id,'original image/file actual read receipt')
        require(self.sql(f'SELECT COUNT(*) FROM chat_message WHERE message_id IN ({image_id},{file_id}) AND displayed_at IS NOT NULL AND status=1') == '2','private receipts persisted')
        receiver['connection'].close()
        offline_uuid=uuid.uuid4().hex
        offline_item={**image_item,'msgid':offline_uuid}
        offline=sender['connection'].exchange(1033,{'fromuid':sender['uid'],'touid':receiver['uid'],'imageArray':[offline_item]},False)
        require(offline.get('error') == 0 and offline.get('persisted'),'actual offline private image persisted')
        request=Request('http://127.0.0.1:38080/user_login',json.dumps({'user':receiver['name'],'passwd':self.wire_password}).encode(),{'Content-Type':'application/json'})
        with urlopen(request,timeout=10) as response:
            login=json.load(response)
        require(login.get('error') == 0,'private receiver actual relogin')
        receiver['token']=login['token'];receiver['connection']=Frames(38091);self.connections.append(receiver['connection'])
        self.issued_tokens.append(receiver['token'])
        require(receiver['connection'].exchange(1005,{'uid':receiver['uid'],'token':receiver['token']},False).get('error') == 0,'private receiver TCP relogin')
        history=receiver['connection'].exchange(1029,{'thread_id':str(thread),'after_message_id':str(image_id-1),'page_size':10},False)
        require(history.get('error') == 0,'offline private history retrieval')
        rows={int(item['message_id']):item for item in history['messages']}
        require(rows[image_id]['message_type'] == 'image' and rows[file_id]['message_type'] == 'file','private history preserves typed resource metadata')
        require(rows[image_id]['status'] == 1 and rows[file_id]['peer_displayed'],'private offline history recovers receipt facts')
        offline_id=int(offline['imageArray'][0]['message_id'])
        require(rows[offline_id]['resource_id'] == image_resource and rows[offline_id]['msgid'] == offline_uuid,'private offline image recovered without live delivery')
        print('PASS actual private image/file cross-server send, trusted metadata, six-byte authenticated complete download, display/read receipts and offline relogin history')

    def negative_avatar_api(self):
        """Send real invalid encoded inputs; every failure preserves published metadata."""
        self.seed()
        group=self.create('a','Actual negative avatar API')
        require(self.mutate('b',1047,group).get('error') == 0,'negative fixture member joins')
        image=Image.new('RGB',(16,16),(40,100,180));output=io.BytesIO();image.save(output,format='PNG')
        valid=output.getvalue()
        original,_=self.upload('a',group,valid)
        require(self.mutate('a',1079,group,resource_id=original).get('error') == 0,'original avatar genuinely published')

        def metadata():
            reply=self.members('a',group)
            require(reply.get('error') == 0,'authoritative avatar metadata snapshot')
            current=reply['group']
            return tuple(current[key] for key in ['avatar_resource_id','avatar_version','avatar_mime_type','avatar_size','avatar_width','avatar_height'])

        original_metadata=metadata()
        def chunk(kind,data):
            return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data)&0xffffffff)
        fake=(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',16,16,8,2,0,0,0))+
              chunk(b'IDAT',zlib.compress(b'\x00'))+chunk(b'IEND',b''))
        output=io.BytesIO();image.save(output,format='GIF');gif=output.getvalue()
        output=io.BytesIO();Image.new('RGB',(1025,1),(20,50,70)).save(output,format='PNG');dimensions=output.getvalue()
        pixels=random.Random(847).randbytes(1024*1024*3)
        output=io.BytesIO();Image.frombytes('RGB',(1024,1024),pixels).save(output,format='PNG');oversize=output.getvalue()
        require(len(oversize)>2*1024*1024,'actual valid encoded PNG exceeds 2MiB')
        for encoded in [gif,dimensions,oversize]:
            decoded=Image.open(io.BytesIO(encoded));decoded.load()
        cases=[('valid_container_invalid_pixels',fake),('unsupported_gif',gif),('valid_png_1025x1',dimensions),('valid_png_1024x1024_over_2MiB',oversize)]
        inputs=self.directory/'negative-avatar-inputs';inputs.mkdir(exist_ok=True)
        for label,data in cases:
            (inputs/(label+'.bin')).write_bytes(data)
            identity=str(uuid.uuid4());self.resources.append(identity)
            request=self.upload_request('a',group,identity,data)
            sync=self.resource(1005,request)
            stage='sync';error=sync.get('error')
            if error == 0:
                stage='final_chunk'
                for offset in range(0,len(data),512):
                    part=data[offset:offset+512]
                    reply=self.resource(1003,{**request,'offset':offset,'data':base64.b64encode(part).decode(),'is_last':offset+len(part)==len(data)})
                    error=reply.get('error')
                    if error != 0:
                        break
            verified=self.verify('a',group,identity)
            published=self.mutate('a',1079,group,resource_id=identity)
            require(error != 0 or verified.error != 0,'invalid actual file rejected by upload or full RPC decoder')
            require(verified.error != 0 and published.get('error') != 0,'invalid actual resource cannot verify or publish')
            require(metadata() == original_metadata,'invalid resource leaves original avatar ID/version/metadata unchanged')
            print(f"PASS actual negative {label} bytes={len(data)} reject_stage={stage} upload_error={error} rpc_error={verified.error} publish_error={published.get('error')} original_unchanged=true")

        denied=str(uuid.uuid4())
        require(self.resource(1005,self.upload_request('b',group,denied,valid)).get('error') != 0,'nonmanager actual upload denied')
        require(self.mutate('b',1079,group,resource_id=original).get('error') != 0,'nonmanager actual publication denied')
        require(metadata() == original_metadata,'nonmanager does not clear or replace original avatar')
        require(self.mutate('a',1069,group,target_member_uid=self.actors['b']['uid'],role=1,expected_membership_version=str(self.version('a','b',group))).get('error') == 0,'fixture manager appointed')
        admin_resource,_=self.upload('b',group,valid)
        require(self.verify('b',group,admin_resource).error == 0,'actual full verification before revocation')
        publish_operation=uuid.uuid4().hex
        require(self.mutate('b',1079,group,resource_id=admin_resource,operation_id=publish_operation).get('error') == 0,'actual manager published own task')
        require(self.mutate('a',1069,group,target_member_uid=self.actors['b']['uid'],role=0,expected_membership_version=str(self.version('a','b',group))).get('error') == 0,'actual manager revoked')
        before_rejected_publish=metadata()
        for fields in [{},{'operation_id':publish_operation}]:
            require(self.mutate('b',1079,group,resource_id=admin_resource,**fields).get('error') != 0,'revoked manager fresh/old successful publication rejected')
            require(metadata() == before_rejected_publish,'revoked failed publication preserves current avatar ID/version/metadata')
        require(self.download('a',group,admin_resource).get('error') == 0,'current publication readable by other member, no uploader-only fallback')
        print('PASS actual nonmanager and revoked fresh/cached publication preserve avatar ID/version/metadata without fallback')

    def announcement_privacy_api(self):
        """Only current members receive body/editor/time/version, including replay/events."""
        self.seed()
        group=self.create('a','Actual member-only announcement')
        join_operation=uuid.uuid4().hex
        require(self.mutate('b',1047,group,operation_id=join_operation).get('error') == 0,'privacy fixture member joins')
        body='Member-only 公告😀\n正文不得出现在公开搜索或待审回包'
        announcement=self.mutate('a',1077,group,announcement=body,expected_announcement_version='0')
        require(announcement.get('error') == 0,'member-only announcement stored')

        def visible(value):
            require(value.get('member') and value.get('announcement_visible'),'current member announcement visible')
            require(value['announcement'] == body and value['announcement_version'] == 1,'current member full announcement body/version')
            require(value['announcement_author_uid'] == self.actors['a']['uid'] and value['announcement_updated_at_ms'] > 0,'current member full editor/time')

        def hidden(value):
            require(not value.get('member') and not value.get('membership_proof'),'nonmember never gains authority from redaction')
            require(value.get('announcement_visible') is False,'public announcement explicit redacted marker')
            require(value['announcement'] == '' and value['announcement_version'] == 0,'nonmember body/version default placeholders')
            require(value['announcement_author_uid'] == 0 and value['announcement_updated_at_ms'] == 0,'nonmember editor/time redacted')
            require(body not in json.dumps(value,ensure_ascii=False),'nonmember JSON contains no current announcement')
            require(int(value['thread_id']) == group and value['name'] == 'Actual member-only announcement' and value['owner_uid'] == self.actors['a']['uid'] and value['active'],'public group lookup retained')

        member=self.actors['b']['connection'].exchange(1045,{'thread_id':str(group)})
        require(member.get('error') == 0,'current member exact search')
        visible(member['group']);visible(self.members('b',group)['group'])
        public=self.actors['f']['connection'].exchange(1045,{'thread_id':str(group)})
        require(public.get('error') == 0,'public exact search still available')
        hidden(public['group'])
        require(self.mutate('a',1075,group,approval_required=True).get('error') == 0,'privacy fixture approval enable')
        pending=self.mutate('d',1047,group,explanation='Pending cannot read member announcement')
        require(pending.get('admission') == 'pending','privacy fixture pending application')
        hidden(pending['group'])
        old_member_version=self.version('a','b',group)
        require(self.mutate('a',1056,group,target_member_uid=self.actors['b']['uid'],expected_membership_version=str(old_member_version)).get('error') == 0,'privacy fixture member kicked')
        invalidated=self.actors['b']['connection'].notification(1060,lambda value:int(value.get('thread_id',0)) == group and value.get('event_type') == 'invalidated')
        require(invalidated.get('uid') == self.actors['b']['uid'] and invalidated.get('type') == 'group','actual invalidation recipient envelope')
        require(int(invalidated['membership_version']) > int(old_member_version),'invalidation retains newer negative membership version')
        hidden(invalidated['group'])
        replay=self.mutate('b',1047,group,operation_id=join_operation)
        require(replay.get('replayed') and not replay.get('membership_granted'),'old JOIN replay cannot grant after kick')
        hidden(replay['group']);visible(self.members('a',group)['group'])
        print('PASS actual member-only announcement full current member search/roster; redacted public search, pending, kicked old JOIN replay and recipient-specific invalidation with unchanged proof boundaries')

    def run(self):
        self.seed()
        group = self.create('a', 'Actual dual service group')
        for actor in ['b', 'c']:
            joined=self.mutate(actor,1047,group)
            require(joined.get('error') == 0, 'direct member join')
            event=self.actors[actor]['connection'].event(self.actors[actor]['uid'],'members',group)
            require(event.get('membership_granted') and event.get('membership_proof'), 'actual same/cross server admission grant')
            while True:
                owner_event=self.actors['a']['connection'].event(self.actors['a']['uid'],'members',group)
                if int(owner_event.get('version',0)) >= int(joined['group']['version']):
                    require(owner_event.get('membership_proof') and not owner_event.get('membership_granted'),'actual member change to owner across both same and other server')
                    break
        invited=self.mutate('a',1061,group,target_member_uid=self.actors['e']['uid'])
        require(invited.get('error') == 0,'approval off direct invitation created')
        invitation=int(invited['entry']['id'])
        accept_operation=uuid.uuid4().hex
        accepted=self.mutate('e',1063,group,invitation_id=str(invitation),accept=True,operation_id=accept_operation)
        require(accepted.get('error') == 0 and accepted['group']['member'],'approval off invitation requires consent then joins')
        require(self.mutate('e',1073,group,expected_membership_version=str(self.version('a','e',group))).get('error') == 0,'direct invite member voluntarily leaves')
        for fields in [{'operation_id':accept_operation},{}]:
            replay=self.mutate('e',1063,group,invitation_id=str(invitation),accept=True,**fields)
            require(not replay.get('membership_granted') and not replay.get('group',{}).get('member'),'same acceptance or fresh operation on terminal invitation cannot rejoin')
        require(self.sql(f"SELECT COUNT(*) FROM group_chat_member WHERE thread_id={group} AND user_id={self.actors['e']['uid']} AND active=1") == '0','terminal invitation never restores retained member')
        require(self.mutate('a',1069,group,target_member_uid=self.actors['b']['uid'],role=1,expected_membership_version=str(self.version('a','b',group))).get('error') == 0,'appoint actual admin')
        require(self.mutate('a',1075,group,approval_required=True).get('error') == 0,'approval enable')
        pending=self.mutate('d',1047,group,explanation='Actual pending application')
        require(pending.get('admission') == 'pending' and not pending.get('membership_granted'), 'pending has no membership')
        application=int(pending['application']['id'])
        for message, payload in [(1053,{'textArray':[{'msgid':uuid.uuid4().hex,'content':'pending must not write'}]}),(1029,{'type':'group','after_message_id':'0','page_size':10})]:
            reply=self.actors['d']['connection'].exchange(message, {'thread_id':str(group),**payload}, message != 1029)
            require(reply.get('error') != 0,'pending text/history denied')
        listing=self.actors['d']['connection'].exchange(1065,{'scope':'applications','after_id':'0','page_size':1})
        require(listing.get('error') == 0 and listing['entries'][0]['status'] == 'pending','actual applications scope retains pending')
        text=self.actors['a']['connection'].exchange(1053,{'thread_id':str(group),'uid':self.actors['f']['uid'],'textArray':[{'msgid':uuid.uuid4().hex,'content':'actual cross server text'}]})
        require(text.get('error') == 0 and text['uid'] == self.actors['a']['uid'],'authenticated text actor')
        require(self.sql(f'SELECT COUNT(*) FROM chat_message WHERE thread_id={group}') == '1','single stored group text, no pending write')
        announcement=self.mutate('b',1077,group,announcement='公告😀\n完整原文',expected_announcement_version='0')
        require(announcement.get('error') == 0 and announcement['group']['announcement'] == '公告😀\n完整原文','actual UTF8 version announcement')
        while True:
            received=self.actors['a']['connection'].event(self.actors['a']['uid'],'metadata',group)
            if received.get('group',{}).get('announcement_version') == 1:
                require(received['group']['announcement'] == '公告😀\n完整原文','actual cross-server metadata delivers complete announcement')
                break
        print('PASS actual same/cross server events, text, pending auth, application scope, announcement')
        if self.management_only:
            return
        image=Image.new('RGB',(16,16),(40,100,180)); buffer=io.BytesIO(); image.save(buffer,format='PNG'); data=buffer.getvalue()
        resource,request=self.upload('a',group,data)
        require(self.verify('a',group,resource,False).error != 0,'valid user token alone is not service identity')
        require(self.verify('a',group,resource,key='wrong-service-key').error != 0,'wrong service credential denied')
        require(self.verify('a',group,resource,token=self.actors['b']['token']).error != 0,'wrong user credential denied at RPC boundary')
        verified=self.verify('a',group,resource)
        require(verified.error == 0 and verified.resource_id == resource and verified.width == 16 and verified.mime_type == 'image/png','real service credential + full metadata verification')
        self.keyless_verification('a',group,resource)
        for downgrade in [{k:v for k,v in request.items() if k not in ['purpose','uid','token','thread_id','resource_id']},{**request,'purpose':'private'},{**request,'uid':self.actors['b']['uid'],'token':self.actors['b']['token']}]:
            require(self.resource(1005,downgrade).get('error') != 0,'protected task resume downgrade/cross-owner denied')
            require(self.resource(1003,{**downgrade,'offset':0,'data':base64.b64encode(data).decode(),'is_last':True}).get('error') != 0,'protected task write downgrade/cross-owner denied')
        base=Path(self.config['ResourceServer']['file_path'])
        if not base.is_absolute():
            base=self.directory/'ResourceServer'/base
        meta_path=base/resource/'meta.json'
        require(meta_path.exists(),'actual completed task metadata exists')
        metadata=json.loads(meta_path.read_text())
        require('token' not in metadata and 'service_key' not in metadata,'no credential persisted in task metadata')
        other=self.create('a','Cross group binding')
        require(self.resource(1005,{**request,'thread_id':str(other)}).get('error') != 0,'authenticated cross-group task resume denied')
        require(self.verify('a',other,resource).error != 0,'cross-group trusted verification denied')
        require(self.mutate('a',1079,other,resource_id=resource).get('error') != 0,'cross-group publication denied')
        published=self.mutate('a',1079,group,resource_id=resource,width=999999,mime_type='untrusted')
        require(published.get('error') == 0 and published['group']['avatar_width'] == 16 and published['group']['avatar_version'] == 1,'publish uses trusted resource metadata')
        require(not any('path' in key or key.endswith('url') for key in published['group']),'group metadata contains no paths')
        require(self.download('d',group,resource).get('error') != 0,'pending avatar read denied')
        require(self.download('f',group,resource).get('error') != 0,'nonmember avatar read denied')
        require(self.download('a',other,resource).get('error') != 0,'cross-group avatar read denied')
        require(self.resource(1007,{'purpose':'group_avatar','uid':self.actors['c']['uid'],'token':self.actors['a']['token'],'thread_id':str(group),'resource_id':resource,'offset':0,'chunk_size':32}).get('error') != 0,'wrong credential denied on download chunk')
        downloaded=b''
        for offset in range(0,len(data),32):
            chunk=self.download('a',group,resource,offset)
            require(chunk.get('error') == 0,'every published download chunk authenticated')
            downloaded+=base64.b64decode(chunk['data'])
        require(downloaded == data,'complete published avatar download matches decoded upload')
        first=self.download('c',group,resource);require(first.get('error') == 0 and base64.b64decode(first['data']) == data[:32],'member first authenticated chunk')
        denied_task=str(uuid.uuid4())
        require(self.resource(1005,self.upload_request('c',group,denied_task,data)).get('error') != 0,'current role0 member cannot upload group avatar')
        leave=self.mutate('c',1073,group,expected_membership_version=str(self.version('a','c',group)))
        require(leave.get('error') == 0,'voluntary member leave')
        require(self.download('c',group,resource,32).get('error') != 0,'next chunk rechecks leave, no reusable download authorization')
        require(self.rpc.VerifyImages(self.pb.ResourceVerifyReq(resource_ids=[resource]),timeout=8).error != 0,'private image verification cannot bypass protected task')
        require(self.rpc.VerifyFiles(self.pb.ResourceFileVerifyReq(resource_ids=[resource]),timeout=8).error != 0,'private file verification cannot bypass protected task')
        admin_resource,admin_request=self.upload('b',group,data)
        require(self.verify('b',group,admin_resource).error == 0,'admin verification before revoke')
        require(self.mutate('a',1069,group,target_member_uid=self.actors['b']['uid'],role=0,expected_membership_version=str(self.version('a','b',group))).get('error') == 0,'revoke admin')
        require(self.mutate('b',1079,group,resource_id=admin_resource).get('error') != 0,'publish rechecks role after prior successful verification')
        require(self.resource(1005,admin_request).get('error') != 0,'revoked uploader cannot resume own task')
        require(self.resource(1003,{**admin_request,'offset':0,'data':base64.b64encode(data).decode(),'is_last':True}).get('error') != 0,'revoked uploader cannot write own next chunk')
        replacement,_=self.upload('a',group,data)
        require(replacement != resource and replacement != admin_resource,'identical bytes do not share account/task identity')
        require(self.mutate('a',1079,group,resource_id=replacement).get('error') == 0,'avatar replacement publication')
        require(self.download('a',group,resource).get('error') != 0,'old published avatar denied every chunk')
        legacy=str(uuid.uuid4());self.resources.append(legacy)
        legacy_request={'upload_id':legacy,'md5':hashlib.md5(data).hexdigest(),'name':'legacy.png','total_size':len(data),'chunk_size':512}
        require(self.resource(1005,legacy_request).get('error') == 0,'legacy anonymous personal/private sync preserved')
        require(self.resource(1003,{**legacy_request,'offset':0,'data':base64.b64encode(data).decode(),'is_last':True}).get('error') == 0,'legacy personal/private upload preserved')
        require(self.verify('a',group,legacy).error != 0,'old anonymous task cannot verify group avatar')
        require(self.rpc.VerifyImages(self.pb.ResourceVerifyReq(resource_ids=[legacy]),timeout=8).error == 0,'legacy private image verification preserved')
        private=self.actors['c']['connection'].exchange(1017,{'fromuid':self.actors['c']['uid'],'touid':self.actors['a']['uid'],'textArray':[{'msgid':uuid.uuid4().hex,'content':'private survives group leave'}]},False)
        require(private.get('error') == 0,'same socket private chat still works after group leave')
        require(self.mutate('b',1067,group,application_id=str(application),approve=True).get('error') != 0,'revoked admin approval denied')
        require(self.mutate('a',1067,group,application_id=str(application),approve=True).get('error') == 0,'owner approves waiting application')
        grant=self.actors['d']['connection'].event(self.actors['d']['uid'],'members',group)
        require(grant.get('membership_granted'),'approved applicant receives explicit grant')
        require(self.mutate('a',1071,group,target_member_uid=self.actors['b']['uid'],expected_membership_version=str(self.version('a','b',group))).get('error') == 0,'actual atomic owner transfer')
        require(self.mutate('b',1073,group,expected_membership_version=str(self.version('b','b',group))).get('error') != 0,'new owner cannot leave')
        require(self.mutate('b',1058,group).get('error') == 0,'new owner dissolve')
        require(self.download('d',group,replacement).get('error') != 0,'dissolved group avatar denied')
        print('PASS actual service credential, task downgrade/cross-owner/cross-group, pending/member auth, revoke-publish, per-chunk leave/current-avatar/dissolve, legacy private regression')
        self.private_resource_regression(legacy,data)
        for name in ['ChatServer1','ChatServer2','ResourceServer']:
            log=(self.directory/(name+'-runtime-persistent.log')).read_text(encoding='utf-8',errors='replace')
            require(log,'actual persistent service diagnostic log exists')
            require(all(value not in log for value in self.issued_tokens+[self.service_key]),'live credential values absent from '+name+' diagnostics')
        print('PASS actual received/upload/download/RPC credentials absent from persistent service diagnostics')

    def cleanup(self):
        if self.keyless_process:
            self.keyless_process.terminate()
            self.keyless_process.wait(timeout=10)
        for connection in self.connections:
            connection.close()
        for group in self.groups:
            self.sql(f'DELETE FROM group_admission WHERE thread_id={group};DELETE FROM chat_message WHERE thread_id={group};DELETE FROM group_chat_member WHERE thread_id={group};DELETE FROM group_chat WHERE thread_id={group};DELETE FROM chat_thread WHERE id={group}')
        self.sql('DELETE FROM group_operation WHERE user_id BETWEEN 1900710001 AND 1900710006')


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--runtime-dir',type=Path,required=True);parser.add_argument('--mysql-client',required=True)
    parser.add_argument('--management-only', action='store_true')
    parser.add_argument('--negative-avatar-only', action='store_true')
    parser.add_argument('--announcement-privacy-only', action='store_true')
    options=parser.parse_args();smoke=Smoke(options.runtime_dir,options.mysql_client)
    smoke.management_only = options.management_only
    try:
        if options.announcement_privacy_only:
            smoke.announcement_privacy_api()
        elif options.negative_avatar_only:
            smoke.negative_avatar_api()
        else:
            smoke.run()
    finally:
        smoke.cleanup()

if __name__ == '__main__':
    main()
