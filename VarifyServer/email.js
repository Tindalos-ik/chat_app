const nodemailer = require('nodemailer');
const config_module = require("./config") //引入配置文件，从而使用config里面的配置

/**
 * 创建发送邮件的代理
 */
let transport = nodemailer.createTransport({
    host: 'smtp.163.com',  // 网易163邮箱的 SMTP 服务器地址: smtp.163.com
    port: 465,
    secure: true,
    auth: {
        user: config_module.email_user, // 发送方邮箱地址
        pass: config_module.email_pass // 邮箱授权码或者密码
    }
});

/**
 * 发送邮件的函数
 * @param {*} mailOptions_ 发送邮件的参数
 * @returns 
 */
function SendMail(mailOptions_){
    return new Promise(function(resolve, reject){
        // 发送邮件, 这个是库函数， 通过回调函数返回结果
        transport.sendMail(mailOptions_, function(error, info){
            if (error) {
                console.log(error);
                reject(error);
            } else {
                console.log('email has ben sent:' + info.response);
                resolve(info.response)
            }
        });
    })
   
}

module.exports.SendMail = SendMail  // 导出发送邮件的函数