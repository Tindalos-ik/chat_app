#include "cserver.h"
#include "AsioIOServicePool.h"
#include "HttpConnection.h"

CServer::CServer(boost::asio::io_context &ioc, unsigned short port): 
    _ioc(ioc),_acceptor(ioc,tcp::endpoint(tcp::v4(),port))
{

}

void CServer::Start(){
    auto self = shared_from_this(); //使用这个可以保证引用计数同步

    //获取一个io_service，用于处理新连接
    auto& io_context = AsioIOServicePool::GetInstance()->GetIOService();
    //创建连接
    std::shared_ptr<HttpConnection> new_con = std::make_shared<HttpConnection>(io_context);

    //传入self指针，保证对象声明周期和回调函数一致，异步回调中使用this指针，this指针不一定存在
    _acceptor.async_accept(new_con->socket(), [self,new_con](boost::system::error_code ec){
        //当有客户端连接时，asio 会从io_context 线程池中调用这个回调函数
        //这个时候，socket才从空壳走向一个真正的socket，asio会把新连接的socket放进去
        try{
            if(ec) {
                //出错放弃这链接，继续监听其他链接
                self->Start(); //这里用self指针，因为self指针是一定会存在的，而this指针不一定存在
                return;
            }
            
            new_con->Start(); //开始处理这个连接
            //至此cserver有一个io_context，专门用于监听，线程池有多个io_context，专门用于处理连接

            //继续监听其他链接
            self->Start();
        }catch(std::exception &e){
            std::cout << e.what() << std::endl;
        }
    });
}