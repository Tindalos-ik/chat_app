const grpc = require('@grpc/grpc-js');
const message_proto = require('./proto');
const emailModule = require('./email');
const const_module = require('./const');
const { v4: uuidv4 } = require('uuid');
const redis_module = require('./redis');

async function GetVarifyCode(call, callback) { //call 是客户端传过来的数据，callback 是返回给客户端的数据
    console.log("email is ", call.request.email)
    try{
        //通过email查询redis中是否有该email，如果没有再创建验证码
        //let显示声明变量，js中是没有类型的
        let query_res = await redis_module.GetRedis(const_module.code_prefix+call.request.email); 
        console.log("query res is ", query_res)
        let uniqueId = '';
        
        // 判断验证码是否已存在且未过期
        if(query_res != null){
            // 验证码已存在，直接返回错误，不重复发送邮件
            console.log("验证码已存在，请勿重复请求");
            callback(null,{
                email: call.request.email,
                error: const_module.Errors.DuplicateRequest  // 需要在 const.js 中定义该错误码
            });
            return;  // 必须 return，防止继续执行
        }
        
        // 验证码不存在，生成新的验证码
        if(query_res == null){
            uniqueId = uuidv4(); //生成唯一id
            if(uniqueId.length > 4){
                uniqueId = uniqueId.substring(0,4); //截取前四位
            }
            let bres = await redis_module.SetRedisExpire(const_module.code_prefix+call.request.email,uniqueId, const_module.expire_time); //将验证码存入redis
            if(!bres){
                callback(null,{
                    email: call.request.email,
                    error:const_module.Errors.Exception
                });//返回错误给客户端
                return;
            }
            console.log("set redis res is ", bres)
            
        }
        //已经存过了，直接返回（这段逻辑已移到上面的 if(query_res != null) 中处理）
        
        //发送邮件
        console.log("uniqueId is ", uniqueId)
        let text_str =  '您的验证码为'+ uniqueId +'，请三分钟内完成操作' //发送文本
        //定义发送邮件，这个格式是固定的，不能改
        let mailOptions = {                
            from: 'tindalos_ovo@163.com',
            to: call.request.email,
            subject: '验证码',
            text: text_str,
        };
    
        let send_res = await emailModule.SendMail(mailOptions); //发送邮件
        console.log("send res is ", send_res)

        if(!send_res){
            callback(null,{
                email: call.request.email,
                error:const_module.Errors.Exception
            })
            return;
        }

        callback(null, { 
            email:  call.request.email,
            error:const_module.Errors.Success
        });    

    }catch(error){
        console.log("catch error is ", error)

        callback(null, { 
            email:  call.request.email,
            error:const_module.Errors.Exception
        }); 
    }
     
}

function main() {
    var server = new grpc.Server()  
    //message.proto 中有：package message; service VarifyService ,后面是 调用服务 : 具体接口，就是我们上面定义的
    server.addService(message_proto.VarifyService.service, { GetVarifyCode: GetVarifyCode })
    server.bindAsync('0.0.0.0:50051', grpc.ServerCredentials.createInsecure(), () => {
        server.start()
        console.log('grpc server started')        
    })
}

main()