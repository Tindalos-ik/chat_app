let code_prefix = "code_"; //发送的验证码前缀

const Errors = { // 错误码
    Success : 0,
    RedisErr : 1,
    Exception : 2,
    DuplicateRequest: 1003 // 请勿重复请求
};

let expire_time = 180; //验证码过期时间，单位为秒


module.exports = {code_prefix,Errors,expire_time} // 导出