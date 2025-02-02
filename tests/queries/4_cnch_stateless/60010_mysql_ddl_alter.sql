use test;
set dialect_type='MYSQL';

drop table if exists tbx;
drop table if exists tbx1;
create table tbx (p int NOT NULL, x tinyint, y String, z float) engine=CnchMergeTree() order by p;

select 'add column';
alter table tbx add column z1 float;
alter table tbx add z2 String NOT NULL;
alter table tbx add column z3 Array(int);
alter table tbx add column z4 DateTime NULL default '2024-02-24 17:00:00';
describe table tbx;

insert into table tbx(p, x, y, z, z1, z2, z3) select number, number, 'hello', 1.0, NULL, 'world', [1,2,3] from system.numbers limit 100;
select * from tbx limit 3;

select 'modify column';
alter table tbx modify column x int NULL;
alter table tbx modify column y String NULL default 'hello';
alter table tbx modify column z double NULL default 1.2;
alter table tbx modify z2 String NULL;
describe table tbx;
select * from tbx limit 3;

alter table tbx modify column z float NULL; -- { serverError 70 }
alter table tbx modify column x decimal(10,3) NULL; -- { serverError 70 }
alter table tbx modify column x String NULL; -- { serverError 70 }
alter table tbx modify column x DateTime NULL; -- { serverError 70 }

alter table tbx modify column z4 Array(float) NULL; -- { serverError 70 }
alter table tbx modify column z4 Array(String) NULL; -- { serverError 70 }
-- alter table tbx modify column x float NOT NULL; -- { serverError 70 }

select 'alter index';
alter table tbx add index idx(y); -- { serverError 524 }
alter table tbx add key idx(y); -- { serverError 524 }
alter table tbx drop index idx; -- { serverError 524 }
-- alter table tbx add clustered key cidx(y); -- {serverError 524 }
-- alter table tbx drop clustered index cidx; -- {serverError 524 }

-- if alter previous columns, there could be exception due to the previous alter ops not finished yet.
-- therefore, create new table with new columns
drop table if exists tbx;
create table tbx (p int NOT NULL, x tinyint, y String, z float) engine=CnchMergeTree() order by p;

insert into table tbx select number, number, 'hello', 1.0 from system.numbers limit 100;
select * from tbx limit 3;

select 'rename column';
alter table tbx rename column x to x1;
describe table tbx;
select * from tbx limit 3;

select 'drop column';
alter table tbx drop column y;
alter table tbx drop z;
describe table tbx;
select * from tbx limit 3;

select 'primary key';
alter table tbx add column z5 int primary key; -- { serverError 524 }
alter table tbx modify column p bigint NULL; -- { serverError 524 }
alter table tbx drop p; -- { serverError 524 }

create table tbx1 (p int NOT NULL, x tinyint, y String, z float) engine=CnchMergeTree() order by p;
select 'rename table';
alter table tbx1 rename tbx11;
alter table tbx11 rename tbx1;
alter table test.tbx1 rename test.tbx11;
alter table test.tbx11 rename test.tbx1;
drop table if exists tbx;
drop table if exists tbx1;
