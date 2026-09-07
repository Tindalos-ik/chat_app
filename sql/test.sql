-- chat_app_db 的可重复执行测试数据。
-- 先执行 create_tables.sql 创建四张表；本脚本不会清空已有数据。
-- 测试账号：test_alice、test_bob、test_carol、test_david、klein 的三个测试好友。
-- 客户端登录密码均为 123456；pwd 保存的是客户端 xorString 后的值 745230。

USE `chat_app_db`;

-- 保证后续注册分配的 uid 不会与本脚本使用的测试 uid 冲突。
INSERT INTO `user_id` (`id`)
SELECT 9103
WHERE NOT EXISTS (SELECT 1 FROM `user_id`);

UPDATE `user_id`
SET `id` = GREATEST(`id`, 9103);

-- 账号资料。uid 和 email 都是唯一键，重复执行时只更新这些固定测试账号。
INSERT INTO `user` (`uid`, `name`, `email`, `pwd`, `nick`, `desc`, `sex`, `icon`)
VALUES
  (9001, 'test_alice', 'test_alice@example.com', '745230', 'Alice',
   'Alice 的测试签名', 1, ':/res/head_1.jpg'),
  (9002, 'test_bob', 'test_bob@example.com', '745230', 'Bob',
   'Bob 的测试签名', 1, ':/res/head_2.jpg'),
  (9003, 'test_carol', 'test_carol@example.com', '745230', 'Carol',
   'Carol 的测试签名', 2, ':/res/head_3.jpg'),
  (9004, 'test_david', 'test_david@example.com', '745230', 'David',
   'David 的测试签名', 1, ':/res/head_4.jpg'),
  (9101, 'test_klein_friend_1', 'test_klein_friend_1@example.com', '745230', '小明',
   'klein 的第一个测试好友', 1, ':/res/head_1.jpg'),
  (9102, 'test_klein_friend_2', 'test_klein_friend_2@example.com', '745230', '小红',
   'klein 的第二个测试好友', 2, ':/res/head_2.jpg'),
  (9103, 'test_klein_friend_3', 'test_klein_friend_3@example.com', '745230', '小刚',
   'klein 的第三个测试好友', 1, ':/res/head_3.jpg')
ON DUPLICATE KEY UPDATE
  `name` = VALUES(`name`),
  `email` = VALUES(`email`),
  `pwd` = VALUES(`pwd`),
  `nick` = VALUES(`nick`),
  `desc` = VALUES(`desc`),
  `sex` = VALUES(`sex`),
  `icon` = VALUES(`icon`);

-- Alice 与 Bob 是已建立好友关系，双向各一行，备注分别属于各自视角。
INSERT INTO `friend` (`self_id`, `friend_id`, `back`)
VALUES
  (9001, 9002, '小鲍勃'),
  (9002, 9001, '爱丽丝')
ON DUPLICATE KEY UPDATE
  `back` = VALUES(`back`);

-- 给已有 klein 用户补三位可显示的好友。user 表提供客户端所需的昵称、头像和签名，
-- friend 表双向各一行，以便三位好友登录后也能看到 klein。
INSERT INTO `friend` (`self_id`, `friend_id`, `back`)
SELECT klein.`uid`, fixture.`friend_id`, fixture.`back`
FROM `user` AS klein
CROSS JOIN (
  SELECT 9101 AS `friend_id`, '测试好友一' AS `back`
  UNION ALL SELECT 9102, '测试好友二'
  UNION ALL SELECT 9103, '测试好友三'
) AS fixture
WHERE klein.`name` = 'klein'
ON DUPLICATE KEY UPDATE
  `back` = VALUES(`back`);

INSERT INTO `friend` (`self_id`, `friend_id`, `back`)
SELECT fixture.`friend_id`, klein.`uid`, 'klein'
FROM `user` AS klein
CROSS JOIN (
  SELECT 9101 AS `friend_id`
  UNION ALL SELECT 9102
  UNION ALL SELECT 9103
) AS fixture
WHERE klein.`name` = 'klein'
ON DUPLICATE KEY UPDATE
  `back` = VALUES(`back`);

-- 覆盖好友申请的三种状态：1=已同意，0=待处理，2=已拒绝。
INSERT INTO `friend_apply` (`from_uid`, `to_uid`, `applicant_remark`, `status`)
VALUES
  (9001, 9002, '小鲍勃', 1),
  (9004, 9001, '大卫同学', 0),
  (9003, 9002, '卡萝尔', 2)
ON DUPLICATE KEY UPDATE
  `applicant_remark` = VALUES(`applicant_remark`),
  `status` = VALUES(`status`);
